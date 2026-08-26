/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "interpreter_common.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "art_field-inl.h"
#include "base/casts.h"
#include "base/pointer_size.h"
#include "base/time_utils.h"
#include "class_linker.h"
#include "class_root-inl.h"
#include "debugger.h"
#include "entrypoints/entrypoint_utils.h"
#include "dex/dex_file_types.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "handle.h"
#include "intrinsics_enum.h"
#include "intrinsics_list.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jni/jni_env_ext.h"
#include "jvalue-inl.h"
#include "method_handles-inl.h"
#include "method_handles.h"
#include "mirror/array-alloc-inl.h"
#include "mirror/array-inl.h"
#include "mirror/call_site-inl.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class.h"
#include "mirror/iftable-inl.h"   // §673: IfTable::GetMethodArrayCount for the bounds check
#include "mirror/field.h"
#include "mirror/emulated_stack_frame.h"
#include "mirror/method.h"
#include "mirror/method_handle_impl-inl.h"
#include "mirror/method_type-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-alloc-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-alloc-inl.h"
#include "mirror/string-inl.h"
#include "mirror/var_handle.h"
#include "nth_caller_visitor.h"
#include "reflection-inl.h"
#include "reflection.h"
#include "scoped_thread_state_change-inl.h"
#include "shadow_frame-inl.h"
#include "stack.h"
#include "thread-inl.h"
#include "var_handles.h"
#include "well_known_classes-inl.h"

#include "westlake_quiet_stdio.h"

namespace art HIDDEN {
namespace interpreter {

static std::atomic<int> g_westlake_main_looper_return_logs{0};
static thread_local ArtMethod* g_westlake_last_main_looper_method = nullptr;
static thread_local ArtMethod* g_westlake_last_main_looper_caller = nullptr;
static thread_local uint32_t g_westlake_last_main_looper_dex_pc = 0;
static thread_local bool g_westlake_last_main_looper_non_null = false;

static bool WestlakeEnvFlagEnabled(const char* name) {
  const char* value = getenv(name);
  return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0 &&
         strcmp(value, "false") != 0 && strcmp(value, "FALSE") != 0 &&
         strcmp(value, "no") != 0 && strcmp(value, "NO") != 0 &&
         strcmp(value, "off") != 0 && strcmp(value, "OFF") != 0;
}

static bool WestlakeTraceTimeZoneBridge() {
  return WestlakeEnvFlagEnabled("WESTLAKE_TRACE_TZ");
}

static bool WestlakeTraceVerboseCalls() {
  return WestlakeEnvFlagEnabled("WESTLAKE_TRACE_VERBOSE_CALLS");
}

static bool WestlakeTraceMcdCalls() {
  return WestlakeEnvFlagEnabled("WESTLAKE_TRACE_MCD_CALLS");
}

// WESTLAKE §752 (2026-08-20): opt-in, method-only trace for Toutiao's short-video
// response handoff.  Transport tracing proved that the tt_video_immerse feed returns
// HTTP 200, while no player data source is installed.  Keep this diagnostic at the ART
// boundary and off by default; it observes managed calls/results without changing them.
static bool WestlakeTraceToutiaoVideoCalls() {
  static const bool enabled = WestlakeEnvFlagEnabled("WESTLAKE_TRACE_TOUTIAO_VIDEO");
  return enabled;
}

// WESTLAKE §753 (2026-08-20): Gson's nextNonWhitespace intrinsic used to
// fflush(stderr) for every JSON token.  A normal short-video feed advanced only
// about 7 KiB per 30 seconds and therefore took hours to reach its callback.
// Keep the diagnostics available on demand without taxing normal execution.
static bool WestlakeTraceGson() {
  static const bool enabled = WestlakeEnvFlagEnabled("WESTLAKE_TRACE_GSON");
  return enabled;
}

static constexpr uintptr_t kPFCutPf625StaleNativeEntry = 0xfffffffffffffb17ULL;

static inline bool PFCutPf625EntryLooksInvalid(const void* entry) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(entry);
  if (value == 0u) {
    return false;
  }
  return value == kPFCutPf625StaleNativeEntry ||
      value < 4096u ||
      (value >> 48u) == 0xffffu ||
      (value & 0x3u) != 0u;
}

static inline void PFCutPf625LogUnsafeNativeEntry(const char* site,
                                                 ArtMethod* method,
                                                 ArtMethod* caller,
                                                 const void* quick_entry,
                                                 const void* jni_entry)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  static thread_local int pf625_log_count = 0;
  if (pf625_log_count >= 120) {
    return;
  }
  pf625_log_count++;
  const std::string method_name = method != nullptr ? method->PrettyMethod() : "<null>";
  const std::string caller_name = caller != nullptr ? caller->PrettyMethod() : "<null>";
  fprintf(stderr,
          "[PFCUT-PF625] unsafe-native-entry site=%s method=%s caller=%s quick=%p jni=%p\n",
          site != nullptr ? site : "<unknown>",
          method_name.c_str(),
          caller_name.c_str(),
          quick_entry,
          jni_entry);
  fflush(stderr);
}

void ThrowNullPointerExceptionFromInterpreter() {
  ThrowNullPointerExceptionFromDexPC();
}

bool CheckStackOverflow(Thread* self, size_t frame_size)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  bool implicit_check = Runtime::Current()->GetImplicitStackOverflowChecks();
  uint8_t* stack_end = self->GetStackEndForInterpreter(implicit_check);
  if (UNLIKELY(__builtin_frame_address(0) < stack_end + frame_size)) {
    ThrowStackOverflowError(self);
    return false;
  }
  return true;
}

bool ShouldStayInSwitchInterpreter(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!Runtime::Current()->IsStarted()) {
    return true;
  }

  if (UNLIKELY(method->IsNative() || method->IsProxyMethod())) {
    return false;
  }

  // PATCH: Always stay in the switch interpreter for non-native methods.
  // Without AOT or JIT, there's no compiled code to call. The Nterp stub
  // is just a 'ret' instruction, and entry points may be unset. Exiting
  // to "compiled code" that doesn't exist causes SIGBUS crashes.
  return true;
}

// WESTLAKE §603f: the §436 guard, ported from the §551 binary code cave into source.
//
// A corrupt ArtMethod reaches the PFCUT predicates below with declaring_class_ holding small garbage
// (observed: 5). The idiomatic `GetDeclaringClass() != nullptr` test PASSES for 5, and then
// DescriptorEquals' inline load at class+0x40 faults at 0x45 -- the §436 launch lottery. §550 also
// showed the ArtMethod pointer itself can be unaligned garbage, and that dropping the alignment test
// merely changes the crash's shape, so BOTH checks are required.
//
// Use this in place of the bare null test on a method's declaring class everywhere in this file.
// It returns false
// for an implausible method or declaring class, which makes the predicate simply not match -- the
// same outcome as a null declaring class, i.e. no behaviour change on healthy input.
static inline bool PFCutDeclaringClassPlausible(ArtMethod* m)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (m == nullptr) return false;
  if ((reinterpret_cast<uintptr_t>(m) & 0x3u) != 0) return false;         // §550: keep the alignment test
  const uintptr_t c = reinterpret_cast<uintptr_t>(m->GetDeclaringClass().Ptr());
  return c >= 0x1000u && (c & 0x3u) == 0;                                 // §551: plausibility test
  // WESTLAKE §634b (2026-08-15) REVERTED: extending this predicate with a dex-index bounds check
  // (idx < dex_file->NumMethodIds()) broke the ZYGOTE — preload died in StreamOpFlag.<clinit>,
  // because PFCUT compat shims that boot depends on stopped matching. Lesson: on this port a
  // method index that is out of range for the dex reached via its dex_cache is apparently NORMAL
  // for classes injected across our patched BCP jars, so "out of range" means "unsafe to READ",
  // NOT "corrupt method". Guard the READ (see WlNameMatches / §632), never the dispatch.
}

// WESTLAKE §673 (2026-08-17): bounds-checked stand-in for
// mirror::Class::FindVirtualMethodForInterface (class-inl.h:589), whose last act is
//
//     iftable->GetMethodArray(i)->GetElementPtrSize<ArtMethod*>(method->GetMethodIndex(), ps)
//
// with NO bounds check. Upstream is entitled to that: matching the interface by identity
// (iftable->GetInterface(i) == declaring_class) is supposed to imply the method array is sized
// for exactly that interface's declared virtual methods -- IfTable::SetMethodArray DCHECKs the
// invariant (iftable-inl.h:68). This port breaks it, because it injects classes across several
// patched BCP jars, so an interface can match by identity while the receiver's method array came
// from a differently-sized copy. GetElementPtrSize is then a raw (array_data + idx * 8) load that
// hands back adjacent heap bytes as an "ArtMethod*".
//
// Measured on Toutiao 2026-08-17 across three consecutive launches: method_index_ 26 read past a
// shorter array returned 0x1000000014 -- non-null, 4-byte aligned, > 0x10000 and < 1<<48, so it
// satisfied every test in the §206 wl_sane_ptr guard below and was then dereferenced by
// PFCutDeclaringClassPlausible() at the §633 site (`ldr w8,[x26]`, libart.so+0x93f250), killing
// the child with SIGSEGV addr=0x1000000014. All three runs faulted at that one instruction; only
// the arrival time varied, which is what made the launch look like a 1-in-3 lottery -- the run
// that reached a window simply crashed 174s in rather than 43s in.
//
// nullptr is already upstream's return value for "no matching interface", so every caller here
// handles it: !wl_sane_ptr takes the §209 repair path, which walks the RECEIVER's own class
// hierarchy for a name+signature match and so needs no iftable indexing at all.
static ArtMethod* WlFindVirtualMethodForInterfaceChecked(ObjPtr<mirror::Class> klass,
                                                         ArtMethod* method,
                                                         PointerSize pointer_size)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (klass == nullptr || method == nullptr) {
    return nullptr;
  }
  ObjPtr<mirror::Class> declaring_class = method->GetDeclaringClass();
  if (declaring_class == nullptr) {
    return nullptr;
  }
  if (UNLIKELY(!declaring_class->IsInterface())) {
    // Upstream delegates to FindVirtualMethodForVirtual() here (Object's public methods).
    // Leave that path exactly as it was.
    return klass->FindVirtualMethodForVirtual(method, pointer_size);
  }
  ObjPtr<mirror::IfTable> iftable = klass->GetIfTable();
  if (iftable == nullptr) {
    return nullptr;
  }
  const int32_t iftable_count = klass->GetIfTableCount();
  const size_t midx = method->GetMethodIndex();
  for (int32_t i = 0; i < iftable_count; i++) {
    if (iftable->GetInterface(i) == declaring_class) {
      const size_t count = iftable->GetMethodArrayCount(i);
      if (UNLIKELY(midx >= count)) {
        // Deliberately NOT getenv()-gated: a diagnostic that never fires teaches nothing.
        // Only integers and the interface descriptor are printed -- naming `method` would mean
        // a dex-backed read on exactly the suspect method that §634 showed can fault.
        static thread_local int wl_ifoob = 0;
        if (wl_ifoob < 20) {
          wl_ifoob++;
          fprintf(stderr,
                  "[WESTLAKE-IFOOB] iface=%s method_index=%zu >= method_array_count=%zu"
                  " (iftable entry %d/%d) -- refused out-of-bounds iftable read\n",
                  declaring_class->PrettyDescriptor().c_str(),
                  midx,
                  count,
                  i,
                  iftable_count);
          fflush(stderr);
        }
        return nullptr;
      }
      return iftable->GetMethodArray(i)->GetElementPtrSize<ArtMethod*>(midx, pointer_size);
    }
  }
  return nullptr;
}

// WESTLAKE §634 (2026-08-15): GetNameView()/GetSignature() index the method's OWN dex by its
// dex_method_index_, and in a release build do so UNCHECKED (the DCHECKs compile away). A method
// that is foreign to its declaring class's dex therefore reads a garbage name_idx and the
// StringId load walks off the mapping — measured on Toutiao: idx 62215 against a dex whose header
// says method_ids_size 41100, name_idx 812,646,432, si_addr == string_ids_ + idx*4.
// Returning false for "cannot read" keeps every caller's intent: `== expected` stays false, and
// `!= expected` (written as !WlNameMatches) stays true, i.e. treated as a mismatch to repair.
static inline bool WlNameMatches(ArtMethod* m, std::string_view expected)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (m == nullptr) {
    return false;
  }
  const uint32_t idx = m->GetDexMethodIndex();
  if (idx == dex::kDexNoIndex) {
    return m->GetNameView() == expected;   // runtime method: GetRuntimeMethodName(), no dex read
  }
  if (!PFCutDeclaringClassPlausible(m)) {
    return false;
  }
  const DexFile* dex_file = m->GetDexFile();
  if (dex_file == nullptr || idx >= dex_file->NumMethodIds()) {
    return false;
  }
  return m->GetNameView() == expected;
}

// WESTLAKE §635 (2026-08-15): may we read this method's dex-backed name/descriptor?
// GetName(), GetNameView(), GetSignature() and GetDeclaringClassDescriptor() all index the
// method's OWN dex by dex_method_index_ and, in a release build, do so UNCHECKED (the DCHECKs in
// art_method-inl.h and DexFile::GetMethodId compile away). Measured on Toutiao (21 dex, Tinker +
// Mira): a method reaches DoCall with index 62215 against a dex whose header says
// method_ids_size 41100, so the MethodId read returns a garbage name_idx (812,646,432 against
// 39,578 strings) and `string_ids_[name_idx]` lands ~3 GB past the mapping.
//
// ⛔Deliberately NOT folded into PFCutDeclaringClassPlausible(): §634b did exactly that to cover
// all ~40 shim sites at once and BROKE THE ZYGOTE (preload died in StreamOpFlag.<clinit>).
// Guard the READ at the site that faults, never the shared dispatch predicate.
//
// Uses GetDexCache() directly rather than ArtMethod::GetDexFile(), which has its own CHECKs and
// obsolete-method handling that we do not want to trigger on a suspect method.
static inline bool WlNameReadable(ArtMethod* m)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (m == nullptr) {
    return false;
  }
  const uint32_t idx = m->GetDexMethodIndex();
  if (idx == dex::kDexNoIndex) {
    return true;   // runtime method: GetRuntimeMethodName(), no dex read
  }
  ObjPtr<mirror::Class> klass = m->GetDeclaringClass();
  const uintptr_t c = reinterpret_cast<uintptr_t>(klass.Ptr());
  if (c < 0x1000u || (c & 0x3u) != 0) {
    return false;
  }
  ObjPtr<mirror::DexCache> dex_cache = klass->GetDexCache();
  if (dex_cache == nullptr) {
    return false;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  return dex_file != nullptr && idx < dex_file->NumMethodIds();
}

// WESTLAKE §635b (2026-08-15): SAFE name/descriptor accessors for the PFCUT shim layer.
//
// Every `strcmp(called_method->GetName(), "…")` in this file reads the method's OWN dex by
// dex_method_index_ and, in a release build, does so UNCHECKED. A method that is foreign to the
// dex reached via its declaring class therefore reads a garbage name_idx and the StringId load
// walks off the mapping — Toutiao hit this twice in a row, first at DoCall+0x38c and then in
// PFCutTryMcdJustFlipEventNoop, both `ldr w9,[x10,x9,lsl #2]` with si_addr ≈ string_ids_ + 3 GB.
//
// Returning "" rather than nullptr is deliberate: every existing `name != nullptr` check keeps
// passing, `strcmp(name,"foo") == 0` becomes false (shim does not match) and `!= 0` becomes true
// (shim bails) — i.e. an unreadable name makes each shim decline, which is always safe because
// they are optional compat shims for specific, known, never-foreign methods.
//
// ⛔Do NOT fold this into PFCutDeclaringClassPlausible(): §634b did that to cover every site at
// once and BROKE THE ZYGOTE (preload died in StreamOpFlag.<clinit>). That predicate also gates
// non-name decisions. Guard the READ, never the shared dispatch predicate.
static inline const char* WlSafeName(ArtMethod* m)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return WlNameReadable(m) ? m->GetName() : "";
}

static inline const char* WlSafeDescriptor(ArtMethod* m)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!WlNameReadable(m)) {
    return "";
  }
  const char* d = m->GetDeclaringClassDescriptor();
  return d != nullptr ? d : "";
}

static inline bool PFCutIsGsonNextNonWhitespace(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return method != nullptr &&
      PFCutDeclaringClassPlausible(method) &&
      (method->GetDeclaringClass()->DescriptorEquals("Lcom/google/gson/stream/JsonReader;") ||
       method->GetDeclaringClass()->DescriptorEquals(
           "Lcom/newrelic/com/google/gson/stream/JsonReader;")) &&
      strcmp(WlSafeName(method), "nextNonWhitespace") == 0;
}

static inline bool PFCutIsGsonJsonReaderMethod(ArtMethod* method, const char* name)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return method != nullptr &&
      PFCutDeclaringClassPlausible(method) &&
      (method->GetDeclaringClass()->DescriptorEquals("Lcom/google/gson/stream/JsonReader;") ||
       method->GetDeclaringClass()->DescriptorEquals(
           "Lcom/newrelic/com/google/gson/stream/JsonReader;")) &&
      strcmp(WlSafeName(method), name) == 0;
}

static inline bool PFCutIsGsonLinkedTreeMapMethod(ArtMethod* method, const char* name)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return method != nullptr &&
      PFCutDeclaringClassPlausible(method) &&
      (method->GetDeclaringClass()->DescriptorEquals("Lcom/google/gson/internal/LinkedTreeMap;") ||
       method->GetDeclaringClass()->DescriptorEquals(
           "Lcom/newrelic/com/google/gson/internal/LinkedTreeMap;")) &&
      strcmp(WlSafeName(method), name) == 0;
}

static inline bool PFCutDecodeHexChar(uint16_t c, uint16_t* value) {
  if (c >= '0' && c <= '9') {
    *value = c - '0';
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    *value = c - 'a' + 10u;
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    *value = c - 'A' + 10u;
    return true;
  }
  return false;
}

static inline int32_t PFCutCompareStrings(ObjPtr<mirror::String> lhs,
                                          ObjPtr<mirror::String> rhs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const int32_t lhs_len = lhs->GetLength();
  const int32_t rhs_len = rhs->GetLength();
  const int32_t min_len = std::min(lhs_len, rhs_len);
  for (int32_t i = 0; i < min_len; ++i) {
    const int32_t diff = static_cast<int32_t>(lhs->CharAt(i)) -
        static_cast<int32_t>(rhs->CharAt(i));
    if (diff != 0) {
      return diff;
    }
  }
  return lhs_len - rhs_len;
}

static inline void PFCutLogGsonReaderState(const char* phase,
                                           ArtMethod* method,
                                           ShadowFrame* frame,
                                           size_t receiver_reg,
                                           JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!WestlakeTraceGson() || !PFCutIsGsonNextNonWhitespace(method) || frame == nullptr) {
    return;
  }
  ObjPtr<mirror::Object> receiver = frame->GetVRegReference(receiver_reg);
  if (receiver == nullptr) {
    fprintf(stderr,
            "[PFCUT-GSON] %s %s receiver=null resultI=%d pending=?\n",
            phase,
            method->PrettyMethod().c_str(),
            result != nullptr ? result->GetI() : 0);
    fflush(stderr);
    return;
  }

  ObjPtr<mirror::Class> klass = receiver->GetClass();
  ArtField* pos_field = klass->FindDeclaredInstanceField("pos", "I");
  ArtField* limit_field = klass->FindDeclaredInstanceField("limit", "I");
  ArtField* line_field = klass->FindDeclaredInstanceField("lineNumber", "I");
  ArtField* line_start_field = klass->FindDeclaredInstanceField("lineStart", "I");
  ArtField* buffer_field = klass->FindDeclaredInstanceField("buffer", "[C");
  const int32_t pos = pos_field != nullptr ? pos_field->GetInt(receiver) : -999;
  const int32_t limit = limit_field != nullptr ? limit_field->GetInt(receiver) : -999;
  const int32_t line = line_field != nullptr ? line_field->GetInt(receiver) : -999;
  const int32_t line_start =
      line_start_field != nullptr ? line_start_field->GetInt(receiver) : -999;
  int32_t buffer_len = -1;
  if (buffer_field != nullptr) {
    ObjPtr<mirror::Object> buffer_obj = buffer_field->GetObject(receiver);
    if (buffer_obj != nullptr && buffer_obj->IsCharArray()) {
      buffer_len = buffer_obj->AsCharArray()->GetLength();
    }
  }

  fprintf(stderr,
          "[PFCUT-GSON] %s %s reader=%p pos=%d limit=%d line=%d lineStart=%d bufferLen=%d resultI=%d\n",
          phase,
          method->PrettyMethod().c_str(),
          receiver.Ptr(),
          pos,
          limit,
          line,
          line_start,
          buffer_len,
          result != nullptr ? result->GetI() : 0);
  fflush(stderr);
}

template <bool is_range>
static inline bool PFCutTryGsonNextNonWhitespace(ArtMethod* called_method,
                                                 ShadowFrame& shadow_frame,
                                                 JValue* result,
                                                 uint16_t number_of_inputs,
                                                 uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                 uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!PFCutIsGsonNextNonWhitespace(called_method) || number_of_inputs != 2u) {
    return false;
  }

  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetI(0);
    return true;
  }

  ObjPtr<mirror::Class> klass = receiver->GetClass();
  ArtField* buffer_field = klass->FindDeclaredInstanceField("buffer", "[C");
  ArtField* pos_field = klass->FindDeclaredInstanceField("pos", "I");
  ArtField* limit_field = klass->FindDeclaredInstanceField("limit", "I");
  ArtField* line_field = klass->FindDeclaredInstanceField("lineNumber", "I");
  ArtField* line_start_field = klass->FindDeclaredInstanceField("lineStart", "I");
  if (buffer_field == nullptr ||
      pos_field == nullptr ||
      limit_field == nullptr ||
      line_field == nullptr ||
      line_start_field == nullptr) {
    return false;
  }

  ObjPtr<mirror::Object> buffer_obj = buffer_field->GetObject(receiver);
  if (buffer_obj == nullptr || !buffer_obj->IsCharArray()) {
    return false;
  }
  ObjPtr<mirror::CharArray> buffer = buffer_obj->AsCharArray();
  const int32_t buffer_len = buffer->GetLength();
  int32_t pos = pos_field->GetInt(receiver);
  const int32_t limit = limit_field->GetInt(receiver);
  int32_t line = line_field->GetInt(receiver);
  int32_t line_start = line_start_field->GetInt(receiver);
  if (pos < 0 || limit < 0 || pos > limit || limit > buffer_len) {
    return false;
  }

  while (pos < limit) {
    const uint16_t c = buffer->GetWithoutChecks(pos);
    const int32_t next_pos = pos + 1;
    if (c == '\n') {
      line++;
      line_start = next_pos;
      pos = next_pos;
      continue;
    }
    if (c == ' ' || c == '\r' || c == '\t') {
      pos = next_pos;
      continue;
    }
    if (c == '/' || c == '#') {
      pos_field->SetInt<false>(receiver, pos);
      line_field->SetInt<false>(receiver, line);
      line_start_field->SetInt<false>(receiver, line_start);
      return false;
    }

    pos_field->SetInt<false>(receiver, next_pos);
    line_field->SetInt<false>(receiver, line);
    line_start_field->SetInt<false>(receiver, line_start);
    result->SetI(static_cast<int32_t>(c));
    if (WestlakeTraceGson()) {
      fprintf(stderr,
              "[PFCUT-GSON] intrinsic nextNonWhitespace reader=%p start=%d end=%d char=%u line=%d lineStart=%d\n",
              receiver.Ptr(),
              pos_field->GetInt(receiver) - 1,
              next_pos,
              static_cast<unsigned int>(c),
              line,
              line_start);
      fflush(stderr);
    }
    return true;
  }

  pos_field->SetInt<false>(receiver, pos);
  line_field->SetInt<false>(receiver, line);
  line_start_field->SetInt<false>(receiver, line_start);
  return false;
}

template <bool is_range>
static inline bool PFCutTryGsonNextQuotedValue(ArtMethod* called_method,
                                               Thread* self,
                                               ShadowFrame& shadow_frame,
                                               JValue* result,
                                               uint16_t number_of_inputs,
                                               uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                               uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!PFCutIsGsonJsonReaderMethod(called_method, "nextQuotedValue") ||
      number_of_inputs != 2u) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LC") != 0) {
    return false;
  }

  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetL(nullptr);
    return true;
  }

  const uint32_t quote_reg = is_range ? vregC + 1u : arg[1];
  const uint16_t quote = static_cast<uint16_t>(shadow_frame.GetVReg(quote_reg));
  if (quote != '"' && quote != '\'') {
    return false;
  }

  ObjPtr<mirror::Class> klass = receiver->GetClass();
  ArtField* buffer_field = klass->FindDeclaredInstanceField("buffer", "[C");
  ArtField* pos_field = klass->FindDeclaredInstanceField("pos", "I");
  ArtField* limit_field = klass->FindDeclaredInstanceField("limit", "I");
  ArtField* line_field = klass->FindDeclaredInstanceField("lineNumber", "I");
  ArtField* line_start_field = klass->FindDeclaredInstanceField("lineStart", "I");
  if (buffer_field == nullptr ||
      pos_field == nullptr ||
      limit_field == nullptr ||
      line_field == nullptr ||
      line_start_field == nullptr) {
    return false;
  }

  ObjPtr<mirror::Object> buffer_obj = buffer_field->GetObject(receiver);
  if (buffer_obj == nullptr || !buffer_obj->IsCharArray()) {
    return false;
  }
  ObjPtr<mirror::CharArray> buffer = buffer_obj->AsCharArray();
  const int32_t buffer_len = buffer->GetLength();
  const int32_t start = pos_field->GetInt(receiver);
  const int32_t limit = limit_field->GetInt(receiver);
  int32_t line = line_field->GetInt(receiver);
  int32_t line_start = line_start_field->GetInt(receiver);
  if (start < 0 || limit < 0 || start > limit || limit > buffer_len) {
    return false;
  }

  std::vector<uint16_t> utf16;
  utf16.reserve(static_cast<size_t>(limit - start));
  int32_t pos = start;
  bool saw_escape = false;
  while (pos < limit) {
    uint16_t c = buffer->GetWithoutChecks(pos++);
    if (c == quote) {
      pos_field->SetInt<false>(receiver, pos);
      line_field->SetInt<false>(receiver, line);
      line_start_field->SetInt<false>(receiver, line_start);
      ObjPtr<mirror::String> string =
          mirror::String::AllocFromUtf16(self,
                                         static_cast<int32_t>(utf16.size()),
                                         utf16.empty() ? nullptr : utf16.data());
      result->SetL(string);

      static thread_local int next_quoted_count = 0;
      if (WestlakeTraceGson() && next_quoted_count < 120) {
        next_quoted_count++;
        fprintf(stderr,
                "[PFCUT-GSON] intrinsic nextQuotedValue reader=%p start=%d end=%d chars=%zu escape=%d\n",
                receiver.Ptr(),
                start,
                pos,
                utf16.size(),
                saw_escape ? 1 : 0);
        fflush(stderr);
      }
      return true;
    }

    if (c == '\n') {
      line++;
      line_start = pos;
      utf16.push_back(c);
      continue;
    }

    if (c != '\\') {
      utf16.push_back(c);
      continue;
    }

    saw_escape = true;
    if (pos >= limit) {
      return false;
    }
    c = buffer->GetWithoutChecks(pos++);
    switch (c) {
      case 'b':
        utf16.push_back('\b');
        break;
      case 'f':
        utf16.push_back('\f');
        break;
      case 'n':
        utf16.push_back('\n');
        break;
      case 'r':
        utf16.push_back('\r');
        break;
      case 't':
        utf16.push_back('\t');
        break;
      case '"':
      case '\'':
      case '/':
      case '\\':
        utf16.push_back(c);
        break;
      case 'u': {
        if (pos + 4 > limit) {
          return false;
        }
        uint16_t value = 0;
        for (int i = 0; i < 4; ++i) {
          uint16_t digit = 0;
          if (!PFCutDecodeHexChar(buffer->GetWithoutChecks(pos++), &digit)) {
            return false;
          }
          value = static_cast<uint16_t>((value << 4) | digit);
        }
        utf16.push_back(value);
        break;
      }
      default:
        return false;
    }
  }

  return false;
}

template <bool is_range>
static inline bool PFCutTryGsonLinkedTreeMapPut(ArtMethod* called_method,
                                                Thread* self,
                                                ShadowFrame& shadow_frame,
                                                JValue* result,
                                                uint16_t number_of_inputs,
                                                uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!PFCutIsGsonLinkedTreeMapMethod(called_method, "put") || number_of_inputs != 3u) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LLL") != 0) {
    return false;
  }

  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  const uint32_t key_reg = is_range ? vregC + 1u : arg[1];
  const uint32_t value_reg = is_range ? vregC + 2u : arg[2];
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
  ObjPtr<mirror::Object> key_obj = shadow_frame.GetVRegReference(key_reg);
  ObjPtr<mirror::Object> value_obj = shadow_frame.GetVRegReference(value_reg);
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetL(nullptr);
    return true;
  }
  if (key_obj == nullptr) {
    self->ThrowNewException("Ljava/lang/NullPointerException;", "key == null");
    result->SetL(nullptr);
    return true;
  }
  if (!key_obj->IsString()) {
    return false;
  }

  ObjPtr<mirror::Class> klass = receiver->GetClass();
  ArtField* allow_null_field = klass->FindInstanceField("allowNullValues", "Z");
  ArtField* size_field = klass->FindInstanceField("size", "I");
  ArtField* mod_count_field = klass->FindInstanceField("modCount", "I");
  ArtField* header_field = nullptr;
  if (allow_null_field == nullptr || size_field == nullptr || mod_count_field == nullptr) {
    return false;
  }

  // Field lookup needs the exact shaded/non-shaded Node descriptor.
  ArtField* header_any = nullptr;
  ArtField* root_any = nullptr;
  for (ArtField& field : klass->GetIFields()) {
    const char* field_name = field.GetName();
    if (strcmp(field_name, "header") == 0) {
      header_any = &field;
    } else if (strcmp(field_name, "root") == 0) {
      root_any = &field;
    }
  }
  if (header_any == nullptr || root_any == nullptr) {
    return false;
  }
  header_field = header_any;
  ArtField* root_field = root_any;

  const bool allow_null = allow_null_field->GetBoolean(receiver);
  if (value_obj == nullptr && !allow_null) {
    self->ThrowNewException("Ljava/lang/NullPointerException;", "value == null");
    result->SetL(nullptr);
    return true;
  }

  ObjPtr<mirror::Object> header = header_field->GetObject(receiver);
  if (header == nullptr) {
    return false;
  }
  ObjPtr<mirror::Class> node_class = header->GetClass();
  std::string node_desc_storage;
  const char* node_desc = node_class->GetDescriptor(&node_desc_storage);
  if (node_desc == nullptr) {
    return false;
  }

  ArtField* parent_field = node_class->FindInstanceField("a", node_desc);
  ArtField* left_field = node_class->FindInstanceField("b", node_desc);
  ArtField* right_field = node_class->FindInstanceField("c", node_desc);
  ArtField* prev_field = node_class->FindInstanceField("d", node_desc);
  ArtField* next_field = node_class->FindInstanceField("e", node_desc);
  ArtField* node_key_field = node_class->FindInstanceField("f", "Ljava/lang/Object;");
  ArtField* node_allow_field = node_class->FindInstanceField("g", "Z");
  ArtField* node_value_field = node_class->FindInstanceField("h", "Ljava/lang/Object;");
  ArtField* height_field = node_class->FindInstanceField("i", "I");
  if (parent_field == nullptr ||
      left_field == nullptr ||
      right_field == nullptr ||
      prev_field == nullptr ||
      next_field == nullptr ||
      node_key_field == nullptr ||
      node_allow_field == nullptr ||
      node_value_field == nullptr ||
      height_field == nullptr) {
    return false;
  }

  ObjPtr<mirror::String> key = key_obj->AsString();
  ObjPtr<mirror::Object> parent = nullptr;
  ObjPtr<mirror::Object> current = root_field->GetObject(receiver);
  int32_t last_compare = 0;
  while (current != nullptr) {
    ObjPtr<mirror::Object> current_key_obj = node_key_field->GetObject(current);
    if (current_key_obj == nullptr || !current_key_obj->IsString()) {
      return false;
    }
    ObjPtr<mirror::String> current_key = current_key_obj->AsString();
    last_compare = PFCutCompareStrings(key, current_key);
    if (last_compare == 0) {
      ObjPtr<mirror::Object> old_value = node_value_field->GetObject(current);
      node_value_field->SetObject<false>(current, value_obj);
      result->SetL(old_value);
      return true;
    }
    parent = current;
    current = last_compare < 0 ? left_field->GetObject(current) : right_field->GetObject(current);
  }

  ObjPtr<mirror::Object> previous = prev_field->GetObject(header);
  if (previous == nullptr) {
    return false;
  }

  StackHandleScope<10> hs(self);
  Handle<mirror::Object> h_receiver(hs.NewHandle(receiver));
  Handle<mirror::Object> h_key(hs.NewHandle(key_obj));
  Handle<mirror::Object> h_value(hs.NewHandle(value_obj));
  Handle<mirror::Object> h_header(hs.NewHandle(header));
  Handle<mirror::Object> h_parent(hs.NewHandle(parent));
  Handle<mirror::Object> h_previous(hs.NewHandle(previous));
  Handle<mirror::Object> h_node(hs.NewHandle(node_class->AllocObject(self)));
  if (h_node == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  parent_field->SetObject<false>(h_node.Get(), h_parent.Get());
  node_key_field->SetObject<false>(h_node.Get(), h_key.Get());
  node_allow_field->SetBoolean<false>(h_node.Get(), allow_null ? 1u : 0u);
  height_field->SetInt<false>(h_node.Get(), 1);
  prev_field->SetObject<false>(h_node.Get(), h_previous.Get());
  next_field->SetObject<false>(h_node.Get(), h_header.Get());
  next_field->SetObject<false>(h_previous.Get(), h_node.Get());
  prev_field->SetObject<false>(h_header.Get(), h_node.Get());
  node_value_field->SetObject<false>(h_node.Get(), h_value.Get());

  if (h_parent == nullptr) {
    root_field->SetObject<false>(h_receiver.Get(), h_node.Get());
  } else if (last_compare < 0) {
    left_field->SetObject<false>(h_parent.Get(), h_node.Get());
  } else {
    right_field->SetObject<false>(h_parent.Get(), h_node.Get());
  }

  size_field->SetInt<false>(h_receiver.Get(), size_field->GetInt(h_receiver.Get()) + 1);
  mod_count_field->SetInt<false>(
      h_receiver.Get(), mod_count_field->GetInt(h_receiver.Get()) + 1);
  result->SetL(nullptr);

  static thread_local int linked_tree_put_count = 0;
  if (WestlakeTraceGson() && linked_tree_put_count < 120) {
    linked_tree_put_count++;
    fprintf(stderr,
            "[PFCUT-GSON] LinkedTreeMap.put intrinsic receiver=%p parent=%p size=%d\n",
            h_receiver.Get(),
            h_parent.Get(),
            size_field->GetInt(h_receiver.Get()));
    fflush(stderr);
  }
  return true;
}

template <bool is_range>
static inline bool PFCutTryStringFactoryCharsetFallback(ArtMethod* called_method,
                                                        Thread* self,
                                                        ShadowFrame& shadow_frame,
                                                        JValue* result,
                                                        uint16_t number_of_inputs,
                                                        uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                        uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/StringFactory;") ||
      strcmp(WlSafeName(called_method), "newStringFromBytes") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr) {
    return false;
  }
  const bool charset_short_form = strcmp(shorty, "LLL") == 0 && number_of_inputs == 2u;
  const bool charset_region_form = strcmp(shorty, "LLIIL") == 0 && number_of_inputs == 4u;
  if (!charset_short_form && !charset_region_form) {
    return false;
  }

  const uint32_t data_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> data_obj = shadow_frame.GetVRegReference(data_reg);
  if (data_obj == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetL(nullptr);
    return true;
  }

  ObjPtr<mirror::ByteArray> byte_array = data_obj->AsByteArray();
  const int32_t data_size = byte_array->GetLength();
  int32_t offset = 0;
  int32_t byte_count = data_size;
  if (charset_region_form) {
    const uint32_t offset_reg = is_range ? vregC + 1u : arg[1];
    const uint32_t count_reg = is_range ? vregC + 2u : arg[2];
    offset = static_cast<int32_t>(shadow_frame.GetVReg(offset_reg));
    byte_count = static_cast<int32_t>(shadow_frame.GetVReg(count_reg));
  }
  if ((offset | byte_count) < 0 || byte_count > data_size - offset) {
    self->ThrowNewExceptionF("Ljava/lang/StringIndexOutOfBoundsException;",
                             "length=%d; regionStart=%d; regionLength=%d",
                             data_size,
                             offset,
                             byte_count);
    result->SetL(nullptr);
    return true;
  }

  static const uint16_t kReplacementChar = 0xfffd;
  static const int kTableUtf8Needed[] = {
      0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };

  std::vector<uint16_t> utf16;
  utf16.reserve(byte_count);
  const int8_t* data = byte_array->GetData();
  int32_t idx = offset;
  const int32_t last = offset + byte_count;
  int32_t code_point = 0;
  int32_t utf8_bytes_seen = 0;
  int32_t utf8_bytes_needed = 0;
  int32_t lower_bound = 0x80;
  int32_t upper_bound = 0xbf;
  while (idx < last) {
    const int32_t b = static_cast<uint8_t>(data[idx++]);
    if (utf8_bytes_needed == 0) {
      if ((b & 0x80) == 0) {
        utf16.push_back(static_cast<uint16_t>(b));
        continue;
      }
      if ((b & 0x40) == 0) {
        utf16.push_back(kReplacementChar);
        continue;
      }

      utf8_bytes_needed = kTableUtf8Needed[b & 0x3f];
      if (utf8_bytes_needed == 0) {
        utf16.push_back(kReplacementChar);
        continue;
      }
      code_point = b & (0x3f >> utf8_bytes_needed);
      if (b == 0xe0) {
        lower_bound = 0xa0;
      } else if (b == 0xed) {
        upper_bound = 0x9f;
      } else if (b == 0xf0) {
        lower_bound = 0x90;
      } else if (b == 0xf4) {
        upper_bound = 0x8f;
      }
    } else {
      if (b < lower_bound || b > upper_bound) {
        utf16.push_back(kReplacementChar);
        code_point = 0;
        utf8_bytes_needed = 0;
        utf8_bytes_seen = 0;
        lower_bound = 0x80;
        upper_bound = 0xbf;
        idx--;
        continue;
      }

      lower_bound = 0x80;
      upper_bound = 0xbf;
      code_point = (code_point << 6) | (b & 0x3f);
      utf8_bytes_seen++;
      if (utf8_bytes_needed != utf8_bytes_seen) {
        continue;
      }
      if (code_point < 0x10000) {
        utf16.push_back(static_cast<uint16_t>(code_point));
      } else {
        utf16.push_back(static_cast<uint16_t>((code_point >> 10) + 0xd7c0));
        utf16.push_back(static_cast<uint16_t>((code_point & 0x3ff) + 0xdc00));
      }
      utf8_bytes_seen = 0;
      utf8_bytes_needed = 0;
      code_point = 0;
    }
  }
  if (utf8_bytes_needed != 0) {
    utf16.push_back(kReplacementChar);
  }

  ObjPtr<mirror::String> string =
      mirror::String::AllocFromUtf16(self,
                                     static_cast<int32_t>(utf16.size()),
                                     utf16.empty() ? nullptr : utf16.data());
  result->SetL(string);

  static thread_local int stringfactory_fallback_count = 0;
  if (stringfactory_fallback_count < 80) {
    stringfactory_fallback_count++;
    fprintf(stderr,
            "[PFCUT] StringFactory Charset fallback bytes=%d offset=%d utf16=%zu shorty=%s\n",
            byte_count,
            offset,
            utf16.size(),
            shorty);
    fflush(stderr);
  }
  return true;
}

template <bool is_range>
static inline bool PFCutTryCharsetNameFallback(ArtMethod* called_method,
                                               Thread* self,
                                               ShadowFrame& shadow_frame,
                                               JValue* result,
                                               uint16_t number_of_inputs,
                                               uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                               uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/nio/charset/Charset;") ||
      strcmp(WlSafeName(called_method), "name") != 0 ||
      number_of_inputs != 1u) {
    return false;
  }

  ObjPtr<mirror::String> name = nullptr;
  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
  if (receiver != nullptr) {
    ArtField* name_field = called_method->GetDeclaringClass()->FindDeclaredInstanceField(
        "name", "Ljava/lang/String;");
    if (name_field != nullptr) {
      ObjPtr<mirror::Object> name_obj = name_field->GetObject(receiver);
      if (name_obj != nullptr && name_obj->IsString()) {
        name = name_obj->AsString();
      }
    }
  }

  if (name == nullptr) {
    name = mirror::String::AllocFromModifiedUtf8(self, "UTF-8");
  }
  result->SetL(name);

  static thread_local int charset_name_fallback_count = 0;
  if (charset_name_fallback_count < 20) {
    charset_name_fallback_count++;
    fprintf(stderr,
            "[PFCUT] Charset.name fallback receiver=%p result=%p\n",
            receiver.Ptr(),
            name.Ptr());
    fflush(stderr);
  }
  return true;
}

static inline ObjPtr<mirror::Object> PFCutGetOrCreateUtf8Charset(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ObjPtr<mirror::Class> charset_class =
      class_linker->FindSystemClass(self, "Ljava/nio/charset/Charset;");
  if (self->IsExceptionPending()) {
    self->ClearException();
  }
  ObjPtr<mirror::Class> standard_charsets_class =
      class_linker->FindSystemClass(self, "Ljava/nio/charset/StandardCharsets;");
  if (self->IsExceptionPending()) {
    self->ClearException();
  }
  if (standard_charsets_class != nullptr) {
    ArtField* utf8_field = standard_charsets_class->FindDeclaredStaticField(
        "UTF_8", "Ljava/nio/charset/Charset;");
    if (utf8_field != nullptr) {
      ObjPtr<mirror::Object> utf8 = utf8_field->GetObject(standard_charsets_class);
      if (utf8 != nullptr) {
        return utf8;
      }
    }
  }

  if (charset_class == nullptr) {
    return nullptr;
  }

  ArtMethod* for_name_method = charset_class->FindClassMethod(
      "forName", "(Ljava/lang/String;)Ljava/nio/charset/Charset;", kRuntimePointerSize);
  if (for_name_method == nullptr) {
    static thread_local int no_for_name_count = 0;
    if (no_for_name_count < 20) {
      no_for_name_count++;
      fprintf(stderr, "[PFCUT] Charset.forName method missing for UTF-8 fallback\n");
      fflush(stderr);
    }
    return nullptr;
  }

  StackHandleScope<6> hs(self);
  Handle<mirror::Class> h_charset_class(hs.NewHandle(charset_class));
  Handle<mirror::Class> h_standard_charsets_class(hs.NewHandle(standard_charsets_class));
  Handle<mirror::String> h_utf8_name(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "UTF-8")));
  if (h_utf8_name == nullptr) {
    return nullptr;
  }

  ScopedObjectAccessUnchecked soa(self);
  ScopedJniEnvLocalRefState env_state(soa.Env());
  jvalue args[1] = {};
  args[0].l = soa.AddLocalReference<jobject>(h_utf8_name.Get());
  JValue for_name_result = InvokeWithJValues(soa, nullptr, for_name_method, args);
  if (self->IsExceptionPending()) {
    static thread_local int for_name_exception_count = 0;
    if (for_name_exception_count < 20) {
      for_name_exception_count++;
      fprintf(stderr, "[PFCUT] Charset.forName(\"UTF-8\") threw during fallback\n");
      fflush(stderr);
    }
    self->ClearException();
    return nullptr;
  }

  Handle<mirror::Object> h_utf8(hs.NewHandle(for_name_result.GetL()));
  if (h_utf8 == nullptr) {
    static thread_local int for_name_null_count = 0;
    if (for_name_null_count < 20) {
      for_name_null_count++;
      fprintf(stderr, "[PFCUT] Charset.forName(\"UTF-8\") returned null\n");
      fflush(stderr);
    }
    return nullptr;
  }

  if (h_standard_charsets_class != nullptr) {
    ArtField* utf8_field = h_standard_charsets_class->FindDeclaredStaticField(
        "UTF_8", "Ljava/nio/charset/Charset;");
    if (utf8_field != nullptr) {
      utf8_field->SetObject<false>(h_standard_charsets_class.Get(), h_utf8.Get());
    }
  }
  ArtField* default_field = h_charset_class->FindDeclaredStaticField(
      "defaultCharset", "Ljava/nio/charset/Charset;");
  if (default_field != nullptr) {
    default_field->SetObject<false>(h_charset_class.Get(), h_utf8.Get());
  }

  static thread_local int utf8_for_name_count = 0;
  if (utf8_for_name_count < 20) {
    utf8_for_name_count++;
    fprintf(stderr, "[PFCUT] Charset.forName(\"UTF-8\") fallback produced %p\n", h_utf8.Get());
    fflush(stderr);
  }
  return h_utf8.Get();
}

static inline ObjPtr<mirror::Object> PFCutGetOrCreateCodingErrorAction(Thread* self,
                                                                       const char* name)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (self == nullptr || name == nullptr) {
    return nullptr;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ObjPtr<mirror::Class> action_class =
      class_linker->FindSystemClass(self, "Ljava/nio/charset/CodingErrorAction;");
  if (self->IsExceptionPending()) {
    self->ClearException();
  }
  if (action_class == nullptr) {
    return nullptr;
  }

  ArtField* static_field = action_class->FindDeclaredStaticField(
      name, "Ljava/nio/charset/CodingErrorAction;");
  if (static_field != nullptr) {
    ObjPtr<mirror::Object> existing = static_field->GetObject(action_class);
    if (existing != nullptr) {
      return existing;
    }
  }

  StackHandleScope<3> hs(self);
  Handle<mirror::Class> h_action_class(hs.NewHandle(action_class));
  Handle<mirror::Object> h_action(hs.NewHandle(h_action_class->AllocObject(self)));
  Handle<mirror::String> h_name(hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, name)));
  if (h_action == nullptr || h_name == nullptr) {
    return nullptr;
  }
  ArtField* name_field = h_action_class->FindInstanceField("name", "Ljava/lang/String;");
  if (name_field != nullptr) {
    name_field->SetObject<false>(h_action.Get(), h_name.Get());
  }
  if (static_field != nullptr) {
    static_field->SetObject<false>(h_action_class.Get(), h_action.Get());
  }
  return h_action.Get();
}

template <bool is_range>
static inline bool PFCutTryCharsetCodingErrorActionFallback(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 2u ||
      called_method->GetDeclaringClass() == nullptr ||
      !(called_method->GetDeclaringClass()->DescriptorEquals("Ljava/nio/charset/CharsetEncoder;") ||
        called_method->GetDeclaringClass()->DescriptorEquals("Ljava/nio/charset/CharsetDecoder;"))) {
    return false;
  }

  const char* method_name = WlSafeName(called_method);
  const bool malformed = method_name != nullptr && strcmp(method_name, "onMalformedInput") == 0;
  const bool unmappable =
      method_name != nullptr && strcmp(method_name, "onUnmappableCharacter") == 0;
  if (!malformed && !unmappable) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  const uint32_t action_reg = is_range ? vregC + 1u : arg[1];
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetL(nullptr);
    return true;
  }
  ArtField* action_field = called_method->GetDeclaringClass()->FindInstanceField(
      malformed ? "malformedInputAction" : "unmappableCharacterAction",
      "Ljava/nio/charset/CodingErrorAction;");
  ObjPtr<mirror::Object> action = shadow_frame.GetVRegReference(action_reg);
  if (action == nullptr) {
    // The platform objects normally already contain REPORT.  Prefer that
    // rooted instance before asking ClassLinker to synthesize one: late video
    // and WebView callbacks can arrive while another exception is pending,
    // in which case FindSystemClass is not a reliable allocation path.
    if (action_field != nullptr) {
      action = action_field->GetObject(receiver);
    }
  }
  if (action == nullptr) {
    action = PFCutGetOrCreateCodingErrorAction(self, "REPORT");
    if (action == nullptr) {
      // This method is itself the compatibility implementation for missing
      // libcore code.  Letting its repair failure escape Looper.loop kills the
      // whole app.  Keep the receiver's prior policy and preserve fluent API
      // semantics; the next encode/decode still reports malformed input.
      if (self->IsExceptionPending()) {
        self->ClearException();
      }
      static thread_local int missing_action_count = 0;
      if (missing_action_count++ < 8) {
        fprintf(stderr,
                "[PFCUT] Charset action unavailable; retained existing policy\n");
        fflush(stderr);
      }
      result->SetL(receiver);
      return true;
    }
  }

  if (action_field != nullptr) {
    action_field->SetObject<false>(receiver, action);
  }

  static thread_local int coding_action_fallback_count = 0;
  if (coding_action_fallback_count < 40) {
    coding_action_fallback_count++;
    fprintf(stderr,
            "[PFCUT] %s.%s fallback action=%p\n",
            WlSafeDescriptor(called_method),
            method_name,
            action.Ptr());
    fflush(stderr);
  }
  result->SetL(receiver);
  return true;
}

static inline bool PFCutUnixPathToHostPath(ObjPtr<mirror::Object> path_object,
                                           std::string* out)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (path_object == nullptr || out == nullptr) {
    return false;
  }
  ArtField* path_field = path_object->GetClass()->FindInstanceField("path", "[B");
  if (path_field == nullptr) {
    return false;
  }
  ObjPtr<mirror::Object> bytes_object = path_field->GetObject(path_object);
  if (bytes_object == nullptr || !bytes_object->IsByteArray()) {
    return false;
  }
  ObjPtr<mirror::ByteArray> bytes = bytes_object->AsByteArray();
  const int32_t length = bytes->GetLength();
  if (length <= 0) {
    *out = ".";
    return true;
  }
  out->assign(reinterpret_cast<const char*>(bytes->GetData()),
              reinterpret_cast<const char*>(bytes->GetData()) + length);
  return true;
}

static inline void PFCutFillUnixFileAttributes(ObjPtr<mirror::Object> attrs,
                                               const struct stat& sb)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (attrs == nullptr || attrs->GetClass() == nullptr) {
    return;
  }
  ObjPtr<mirror::Class> cls = attrs->GetClass();
  const auto set_int = [&](const char* name, int32_t value) {
    ArtField* field = cls->FindInstanceField(name, "I");
    if (field != nullptr) {
      field->SetInt<false>(attrs, value);
    }
  };
  const auto set_long = [&](const char* name, int64_t value) {
    ArtField* field = cls->FindInstanceField(name, "J");
    if (field != nullptr) {
      field->SetLong<false>(attrs, value);
    }
  };

  set_int("st_mode", static_cast<int32_t>(sb.st_mode));
  set_long("st_ino", static_cast<int64_t>(sb.st_ino));
  set_long("st_dev", static_cast<int64_t>(sb.st_dev));
  set_long("st_rdev", static_cast<int64_t>(sb.st_rdev));
  set_int("st_nlink", static_cast<int32_t>(sb.st_nlink));
  set_int("st_uid", static_cast<int32_t>(sb.st_uid));
  set_int("st_gid", static_cast<int32_t>(sb.st_gid));
  set_long("st_size", static_cast<int64_t>(sb.st_size));
  set_long("st_atime_sec", static_cast<int64_t>(sb.st_atime));
  set_long("st_mtime_sec", static_cast<int64_t>(sb.st_mtime));
  set_long("st_ctime_sec", static_cast<int64_t>(sb.st_ctime));
#if defined(__APPLE__)
  set_long("st_atime_nsec", static_cast<int64_t>(sb.st_atimespec.tv_nsec));
  set_long("st_mtime_nsec", static_cast<int64_t>(sb.st_mtimespec.tv_nsec));
  set_long("st_ctime_nsec", static_cast<int64_t>(sb.st_ctimespec.tv_nsec));
  set_long("st_birthtime_sec", static_cast<int64_t>(sb.st_birthtimespec.tv_sec));
#else
  set_long("st_atime_nsec", static_cast<int64_t>(sb.st_atim.tv_nsec));
  set_long("st_mtime_nsec", static_cast<int64_t>(sb.st_mtim.tv_nsec));
  set_long("st_ctime_nsec", static_cast<int64_t>(sb.st_ctim.tv_nsec));
  set_long("st_birthtime_sec", static_cast<int64_t>(sb.st_mtime));
#endif
}

template <bool is_range>
static inline bool PFCutTryUnixNativeDispatcherPathIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Lsun/nio/fs/UnixNativeDispatcher;")) {
    return false;
  }

  const char* method_name = WlSafeName(called_method);
  if (method_name == nullptr) {
    return false;
  }
  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr) {
    return false;
  }
  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  const bool stat_attrs =
      number_of_inputs == 2u && strcmp(shorty, "VLL") == 0 &&
      (strcmp(method_name, "stat") == 0 || strcmp(method_name, "lstat") == 0);
  const bool stat_mode =
      number_of_inputs == 1u && strcmp(shorty, "IL") == 0 &&
      strcmp(method_name, "stat") == 0;
  const bool exists =
      number_of_inputs == 1u && strcmp(shorty, "ZL") == 0 &&
      strcmp(method_name, "exists") == 0;
  const bool access_check =
      number_of_inputs == 2u && strcmp(shorty, "VLI") == 0 &&
      strcmp(method_name, "access") == 0;
  if (!stat_attrs && !stat_mode && !exists && !access_check) {
    return false;
  }

  ObjPtr<mirror::Object> path_object = shadow_frame.GetVRegReference(reg_at(0));
  if (path_object == nullptr) {
    ThrowNullPointerException("path == null");
    result->SetJ(0);
    return true;
  }
  std::string path;
  if (!PFCutUnixPathToHostPath(path_object, &path) || path.empty()) {
    self->ThrowNewException("Lsun/nio/fs/UnixException;", "invalid path");
    result->SetJ(0);
    return true;
  }

  struct stat sb;
  if (stat_attrs) {
    ObjPtr<mirror::Object> attrs = shadow_frame.GetVRegReference(reg_at(1));
    if (attrs == nullptr) {
      ThrowNullPointerException("attrs == null");
      result->SetJ(0);
      return true;
    }
    const int rc = strcmp(method_name, "lstat") == 0 ?
        lstat(path.c_str(), &sb) : stat(path.c_str(), &sb);
    if (rc < 0) {
      self->ThrowNewExceptionF("Lsun/nio/fs/UnixException;", "%s", strerror(errno));
      result->SetJ(0);
      return true;
    }
    PFCutFillUnixFileAttributes(attrs, sb);
    result->SetJ(0);
  } else if (stat_mode) {
    if (stat(path.c_str(), &sb) < 0) {
      result->SetI(0);
    } else {
      result->SetI(static_cast<int32_t>(sb.st_mode));
    }
  } else if (exists) {
    result->SetZ(access(path.c_str(), F_OK) == 0);
  } else {
    const int32_t mode = static_cast<int32_t>(shadow_frame.GetVReg(reg_at(1)));
    if (access(path.c_str(), mode) < 0) {
      self->ThrowNewExceptionF("Lsun/nio/fs/UnixException;", "%s", strerror(errno));
      result->SetJ(0);
      return true;
    }
    result->SetJ(0);
  }

  static thread_local int unix_native_path_intrinsic_count = 0;
  if (unix_native_path_intrinsic_count < 80) {
    unix_native_path_intrinsic_count++;
    fprintf(stderr,
            "[PFCUT] UnixNativeDispatcher.%s intrinsic path=%s shorty=%s\n",
            method_name,
            path.c_str(),
            shorty);
    fflush(stderr);
  }
  return true;
}

template <bool is_range>
static inline bool PFCutTryZipFileRequireNonNullCharsetFallback(
    ArtMethod* called_method,
    ArtMethod* caller_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/util/Objects;") ||
      strcmp(WlSafeName(called_method), "requireNonNull") != 0 ||
      number_of_inputs != 2u) {
    return false;
  }

  const uint32_t value_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> value = shadow_frame.GetVRegReference(value_reg);
  if (value != nullptr) {
    return false;
  }

  const uint32_t message_reg = is_range ? vregC + 1u : arg[1];
  ObjPtr<mirror::Object> message_obj = shadow_frame.GetVRegReference(message_reg);
  if (message_obj == nullptr ||
      message_obj->GetClass() == nullptr ||
      !message_obj->GetClass()->DescriptorEquals("Ljava/lang/String;")) {
    return false;
  }
  std::string message = message_obj->AsString()->ToModifiedUtf8();
  if (message != "charset") {
    return false;
  }

  ObjPtr<mirror::Object> utf8 = PFCutGetOrCreateUtf8Charset(self);
  if (utf8 == nullptr) {
    static thread_local int zip_charset_null_utf8_count = 0;
    if (zip_charset_null_utf8_count < 20) {
      zip_charset_null_utf8_count++;
      fprintf(stderr,
              "[PFCUT] ZipFile charset fallback failed to synthesize UTF-8 caller=%s\n",
              caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>");
      fflush(stderr);
    }
    return false;
  }

  static thread_local int zip_charset_fallback_count = 0;
  if (zip_charset_fallback_count < 40) {
    zip_charset_fallback_count++;
    fprintf(stderr,
            "[PFCUT] ZipFile null charset fallback -> UTF-8 caller=%s\n",
            caller_method->PrettyMethod().c_str());
    fflush(stderr);
  }
  result->SetL(utf8);
  return true;
}

template <bool is_range>
static inline void PFCutNormalizeZipFileNullCharsetArg(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/util/zip/ZipFile;") ||
      strcmp(WlSafeName(called_method), "<init>") != 0 ||
      (number_of_inputs != 4u && number_of_inputs != 5u)) {
    return;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr ||
      (strcmp(shorty, "VLIL") != 0 && strcmp(shorty, "VLILZ") != 0)) {
    return;
  }

  const uint32_t charset_reg = is_range ? vregC + 3u : arg[3];
  if (shadow_frame.GetVRegReference(charset_reg) != nullptr) {
    return;
  }

  ObjPtr<mirror::Object> utf8 = PFCutGetOrCreateUtf8Charset(self);
  if (utf8 == nullptr) {
    static thread_local int zip_arg_utf8_fail_count = 0;
    if (zip_arg_utf8_fail_count < 20) {
      zip_arg_utf8_fail_count++;
      fprintf(stderr,
              "[PFCUT] ZipFile null charset arg normalization failed shorty=%s inputs=%u\n",
              shorty,
              number_of_inputs);
      fflush(stderr);
    }
    return;
  }

  shadow_frame.SetVRegReference(charset_reg, utf8);
  static thread_local int zip_arg_normalize_count = 0;
  if (zip_arg_normalize_count < 40) {
    zip_arg_normalize_count++;
    fprintf(stderr,
            "[PFCUT] ZipFile null charset arg normalized -> UTF-8 shorty=%s inputs=%u\n",
            shorty,
            number_of_inputs);
    fflush(stderr);
  }
}

static inline void PFCutPrintObjectSummary(ObjPtr<mirror::Object> object, int depth)
    REQUIRES_SHARED(Locks::mutator_lock_);

template <bool is_range>
static inline bool PFCutTryThreadGroupUncaughtExceptionNoop(ArtMethod* called_method,
                                                            ShadowFrame& shadow_frame,
                                                            JValue* result,
                                                            uint16_t number_of_inputs,
                                                            uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                            uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/ThreadGroup;") ||
      strcmp(WlSafeName(called_method), "uncaughtException") != 0 ||
      number_of_inputs != 3u) {
    return false;
  }

  result->SetJ(0);
  static thread_local int uncaught_noop_count = 0;
  if (uncaught_noop_count < 40) {
    uncaught_noop_count++;
    const uint32_t thread_reg = is_range ? vregC + 1u : arg[1];
    const uint32_t throwable_reg = is_range ? vregC + 2u : arg[2];
    ObjPtr<mirror::Object> thread_obj = shadow_frame.GetVRegReference(thread_reg);
    ObjPtr<mirror::Object> throwable_obj = shadow_frame.GetVRegReference(throwable_reg);
    ArtMethod* caller_method = shadow_frame.GetMethod();
    fprintf(stderr,
            "[PFCUT] ThreadGroup.uncaughtException noop caller=%s callee=%s thread=",
            caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
            called_method->PrettyMethod().c_str());
    PFCutPrintObjectSummary(thread_obj, 0);
    fprintf(stderr, " throwable=");
    PFCutPrintObjectSummary(throwable_obj, 0);
    fprintf(stderr, "\n");
    fflush(stderr);
  }
  return true;
}

template <bool is_range>
static inline bool PFCutTryStringIntrinsic(ArtMethod* called_method,
                                           Thread* self,
                                           ShadowFrame& shadow_frame,
                                           JValue* result,
                                           uint16_t number_of_inputs,
                                           uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                           uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/String;")) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  const char* name = WlSafeName(called_method);
  if (name == nullptr || shorty == nullptr) {
    return false;
  }

  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> receiver_obj = shadow_frame.GetVRegReference(receiver_reg);
  if (receiver_obj == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }

  ObjPtr<mirror::String> str = receiver_obj->AsString();
  if (strcmp(name, "getBytes") == 0 &&
      strcmp(shorty, "LL") == 0 &&
      number_of_inputs == 2u) {
    const uint32_t charset_reg = is_range ? vregC + 1u : arg[1];
    ObjPtr<mirror::Object> charset = shadow_frame.GetVRegReference(charset_reg);
    if (charset != nullptr) {
      return false;
    }

    StackHandleScope<1> hs(self);
    Handle<mirror::String> h_str(hs.NewHandle(str));
    const int32_t length = h_str->GetLength();
    int32_t byte_count = 0;
    if (h_str->IsCompressed()) {
      byte_count = length;
    } else {
      for (int32_t i = 0; i < length; ++i) {
        const uint16_t ch = h_str->CharAt(i);
        if (ch <= 0x7f) {
          byte_count += 1;
        } else if (ch <= 0x7ff) {
          byte_count += 2;
        } else if (ch >= 0xd800 && ch <= 0xdbff &&
                   i + 1 < length &&
                   h_str->CharAt(i + 1) >= 0xdc00 &&
                   h_str->CharAt(i + 1) <= 0xdfff) {
          byte_count += 4;
          i++;
        } else {
          byte_count += 3;
        }
      }
    }

    ObjPtr<mirror::ByteArray> bytes = mirror::ByteArray::Alloc(self, byte_count);
    if (bytes == nullptr) {
      result->SetL(nullptr);
      return true;
    }

    int8_t* out = bytes->GetData();
    if (h_str->IsCompressed()) {
      std::memcpy(out, h_str->GetValueCompressed(), byte_count);
    } else {
      int32_t out_index = 0;
      for (int32_t i = 0; i < length; ++i) {
        uint32_t cp = h_str->CharAt(i);
        if (cp >= 0xd800 && cp <= 0xdbff &&
            i + 1 < length &&
            h_str->CharAt(i + 1) >= 0xdc00 &&
            h_str->CharAt(i + 1) <= 0xdfff) {
          const uint32_t low = h_str->CharAt(i + 1);
          cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
          i++;
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
          cp = 0xfffd;
        }

        if (cp <= 0x7f) {
          out[out_index++] = static_cast<int8_t>(cp);
        } else if (cp <= 0x7ff) {
          out[out_index++] = static_cast<int8_t>(0xc0 | (cp >> 6));
          out[out_index++] = static_cast<int8_t>(0x80 | (cp & 0x3f));
        } else if (cp <= 0xffff) {
          out[out_index++] = static_cast<int8_t>(0xe0 | (cp >> 12));
          out[out_index++] = static_cast<int8_t>(0x80 | ((cp >> 6) & 0x3f));
          out[out_index++] = static_cast<int8_t>(0x80 | (cp & 0x3f));
        } else {
          out[out_index++] = static_cast<int8_t>(0xf0 | (cp >> 18));
          out[out_index++] = static_cast<int8_t>(0x80 | ((cp >> 12) & 0x3f));
          out[out_index++] = static_cast<int8_t>(0x80 | ((cp >> 6) & 0x3f));
          out[out_index++] = static_cast<int8_t>(0x80 | (cp & 0x3f));
        }
      }
    }

    static thread_local int string_get_bytes_fallback_count = 0;
    if (string_get_bytes_fallback_count < 40) {
      string_get_bytes_fallback_count++;
      fprintf(stderr,
              "[PFCUT] String.getBytes null Charset fallback UTF-8 chars=%d bytes=%d\n",
              length,
              byte_count);
      fflush(stderr);
    }
    result->SetL(bytes);
    return true;
  }

  if (strcmp(name, "length") == 0 && strcmp(shorty, "I") == 0 && number_of_inputs == 1u) {
    result->SetI(str->GetLength());
    return true;
  }

  if (strcmp(name, "hashCode") == 0 && strcmp(shorty, "I") == 0 && number_of_inputs == 1u) {
    int32_t hash = 0;
    const int32_t length = str->GetLength();
    for (int32_t i = 0; i < length; ++i) {
      hash = hash * 31 + str->CharAt(i);
    }
    result->SetI(hash);
    return true;
  }

  if (strcmp(name, "charAt") == 0 && strcmp(shorty, "CI") == 0 && number_of_inputs == 2u) {
    const uint32_t index_reg = is_range ? vregC + 1u : arg[1];
    const int32_t index = static_cast<int32_t>(shadow_frame.GetVReg(index_reg));
    const int32_t length = str->GetLength();
    if (UNLIKELY(index < 0 || index >= length)) {
      ThrowStringIndexOutOfBoundsException(index, length);
      result->SetJ(0);
      return true;
    }
    result->SetC(str->CharAt(index));
    return true;
  }

  if (strcmp(name, "indexOf") == 0 &&
      (strcmp(shorty, "II") == 0 || strcmp(shorty, "III") == 0) &&
      (number_of_inputs == 2u || number_of_inputs == 3u)) {
    const uint32_t ch_reg = is_range ? vregC + 1u : arg[1];
    const int32_t ch = static_cast<int32_t>(shadow_frame.GetVReg(ch_reg));
    int32_t from_index = 0;
    if (number_of_inputs == 3u) {
      const uint32_t from_reg = is_range ? vregC + 2u : arg[2];
      from_index = static_cast<int32_t>(shadow_frame.GetVReg(from_reg));
    }

    const int32_t length = str->GetLength();
    if (from_index < 0) {
      from_index = 0;
    }
    if (from_index >= length || ch < 0 || ch > 0xffff) {
      result->SetI(-1);
      return true;
    }
    for (int32_t i = from_index; i < length; ++i) {
      if (str->CharAt(i) == static_cast<uint16_t>(ch)) {
        result->SetI(i);
        return true;
      }
    }
    result->SetI(-1);
    return true;
  }

  if (strcmp(name, "lastIndexOf") == 0 &&
      (strcmp(shorty, "II") == 0 || strcmp(shorty, "III") == 0) &&
      (number_of_inputs == 2u || number_of_inputs == 3u)) {
    const uint32_t ch_reg = is_range ? vregC + 1u : arg[1];
    const int32_t ch = static_cast<int32_t>(shadow_frame.GetVReg(ch_reg));
    int32_t from_index = str->GetLength() - 1;
    if (number_of_inputs == 3u) {
      const uint32_t from_reg = is_range ? vregC + 2u : arg[2];
      from_index = static_cast<int32_t>(shadow_frame.GetVReg(from_reg));
    }

    const int32_t length = str->GetLength();
    if (from_index >= length) {
      from_index = length - 1;
    }
    if (from_index < 0 || ch < 0 || ch > 0xffff) {
      result->SetI(-1);
      return true;
    }
    for (int32_t i = from_index; i >= 0; --i) {
      if (str->CharAt(i) == static_cast<uint16_t>(ch)) {
        result->SetI(i);
        return true;
      }
    }
    result->SetI(-1);
    return true;
  }

  if (strcmp(name, "split") == 0 &&
      ((strcmp(shorty, "LLI") == 0 && number_of_inputs == 3u) ||
       (strcmp(shorty, "LL") == 0 && number_of_inputs == 2u))) {
    const uint32_t regex_reg = is_range ? vregC + 1u : arg[1];
    ObjPtr<mirror::Object> regex_obj = shadow_frame.GetVRegReference(regex_reg);
    if (regex_obj == nullptr) {
      ThrowNullPointerExceptionFromInterpreter();
      result->SetL(nullptr);
      return true;
    }

    ObjPtr<mirror::String> regex = regex_obj->AsString();
    if (regex->GetLength() != 1) {
      return false;
    }

    const uint16_t delimiter = regex->CharAt(0);
    switch (delimiter) {
      case '.':
      case '^':
      case '$':
      case '|':
      case '?':
      case '*':
      case '+':
      case '(':
      case ')':
      case '[':
      case '{':
      case '\\':
        return false;
      default:
        break;
    }

    int32_t limit = 0;
    if (number_of_inputs == 3u) {
      const uint32_t limit_reg = is_range ? vregC + 2u : arg[2];
      limit = static_cast<int32_t>(shadow_frame.GetVReg(limit_reg));
    }

    const int32_t length = str->GetLength();
    std::vector<std::pair<int32_t, int32_t>> pieces;
    pieces.reserve(4);
    int32_t start = 0;
    bool matched = false;
    for (int32_t i = 0; i < length; ++i) {
      const bool can_split_more = limit <= 0 || static_cast<int32_t>(pieces.size()) < limit - 1;
      if (can_split_more && str->CharAt(i) == delimiter) {
        pieces.emplace_back(start, i - start);
        start = i + 1;
        matched = true;
      }
    }
    pieces.emplace_back(start, length - start);
    if (limit == 0 && matched) {
      while (!pieces.empty() && pieces.back().second == 0) {
        pieces.pop_back();
      }
    }

    StackHandleScope<2> hs(self);
    Handle<mirror::String> h_str(hs.NewHandle(str));
    Handle<mirror::ObjectArray<mirror::String>> h_array(hs.NewHandle(
        mirror::ObjectArray<mirror::String>::Alloc(
            self,
            GetClassRoot<mirror::ObjectArray<mirror::String>>(),
            static_cast<int32_t>(pieces.size()))));
    if (h_array == nullptr) {
      result->SetL(nullptr);
      return true;
    }

    gc::AllocatorType allocator = Runtime::Current()->GetHeap()->GetCurrentAllocator();
    for (int32_t i = 0; i < static_cast<int32_t>(pieces.size()); ++i) {
      ObjPtr<mirror::String> part =
          mirror::String::AllocFromString(self, pieces[i].second, h_str, pieces[i].first, allocator);
      if (part == nullptr) {
        result->SetL(nullptr);
        return true;
      }
      h_array->SetWithoutChecks</*kTransactionActive=*/ false, /*kCheckTransaction=*/ false>(
          i, part);
    }

    static thread_local int split_intrinsic_count = 0;
    if (split_intrinsic_count < 80) {
      split_intrinsic_count++;
      fprintf(stderr,
              "[PFCUT] String.split intrinsic delimiter=%u limit=%d pieces=%zu\n",
              static_cast<unsigned>(delimiter),
              limit,
              pieces.size());
      fflush(stderr);
    }

    result->SetL(h_array.Get());
    return true;
  }

  return false;
}

static inline bool PFCutTryUuidIntrinsic(ArtMethod* called_method,
                                         Thread* self,
                                         JValue* result,
                                         uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 0u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/util/UUID;") ||
      strcmp(WlSafeName(called_method), "randomUUID") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "L") != 0) {
    return false;
  }

  ObjPtr<mirror::Class> uuid_class = called_method->GetDeclaringClass();
  ObjPtr<mirror::Object> uuid = uuid_class->AllocObject(self);
  if (uuid == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  static uint64_t uuid_counter = 1;
  const uint64_t sequence = uuid_counter++;
  const uint64_t most_sig_bits = 0x123456789abc4000ULL | (sequence & 0x0fffULL);
  const uint64_t least_sig_bits = 0x8000000000000000ULL | (sequence & 0x3fffffffffffffffULL);
  ArtField* most = uuid_class->FindInstanceField("mostSigBits", "J");
  ArtField* least = uuid_class->FindInstanceField("leastSigBits", "J");
  if (most == nullptr || least == nullptr) {
    result->SetL(nullptr);
    return true;
  }
  most->SetLong<false>(uuid, static_cast<int64_t>(most_sig_bits));
  least->SetLong<false>(uuid, static_cast<int64_t>(least_sig_bits));
  result->SetL(uuid);
  return true;
}

static inline bool PFCutTrySystemTimeIntrinsic(ArtMethod* called_method,
                                               JValue* result,
                                               uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 0u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/System;")) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  const char* name = WlSafeName(called_method);
  if (name == nullptr || shorty == nullptr || strcmp(shorty, "J") != 0) {
    return false;
  }

  if (strcmp(name, "nanoTime") == 0) {
    result->SetJ(static_cast<int64_t>(NanoTime()));
    return true;
  }
  if (strcmp(name, "currentTimeMillis") == 0) {
    // System.currentTimeMillis() is wall-clock time since the Unix epoch.
    // ART's MilliTime() deliberately uses CLOCK_MONOTONIC on Linux and is
    // suitable for elapsed durations only.  Returning it here made Java
    // dates land near January 1970 and caused valid X.509 leaves to fail as
    // "not yet valid".  Keep nanoTime() monotonic above, but use realtime
    // for this separate Java contract.
    timeval now;
    if (gettimeofday(&now, nullptr) == 0) {
      result->SetJ(static_cast<int64_t>(now.tv_sec) * INT64_C(1000) +
                   static_cast<int64_t>(now.tv_usec) / INT64_C(1000));
    } else {
      result->SetJ(static_cast<int64_t>(time(nullptr)) * INT64_C(1000));
    }
    return true;
  }
  return false;
}

static inline void PFCutThrowArrayStoreExceptionNotAnArray(const char* identifier,
                                                           ObjPtr<mirror::Object> object)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string actual_type(mirror::Object::PrettyTypeOf(object));
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
                                        "%s of type %s is not an array",
                                        identifier,
                                        actual_type.c_str());
}

static inline bool PFCutArraycopyElementAssignable(ObjPtr<mirror::Class> dst_component_type,
                                                   const char* dst_array_descriptor,
                                                   ObjPtr<mirror::Object> element)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (element == nullptr) {
    return true;
  }
  ObjPtr<mirror::Class> element_class = element->GetClass();
  if (dst_component_type != nullptr && dst_component_type->IsAssignableFrom(element_class)) {
    return true;
  }

  std::string element_desc_storage;
  const char* element_desc = element_class->GetDescriptor(&element_desc_storage);
  if (dst_component_type != nullptr && element_desc != nullptr) {
    std::string component_desc_storage;
    const char* component_desc = dst_component_type->GetDescriptor(&component_desc_storage);
    if (component_desc != nullptr && strcmp(component_desc, element_desc) == 0) {
      return true;
    }
  }

  // Westlake still has a few duplicate/incorrect component-class identities while
  // bootstrapping app class loaders. The array descriptor is authoritative for
  // the runtime store target, so use it as a portable fallback before throwing.
  if (dst_array_descriptor != nullptr && dst_array_descriptor[0] == '[' && element_desc != nullptr) {
    const char* expected_component_desc = dst_array_descriptor + 1;
    if (strcmp(expected_component_desc, "Ljava/lang/Object;") == 0 ||
        strcmp(expected_component_desc, element_desc) == 0) {
      return true;
    }
  }
  return false;
}

static inline bool PFCutObjectArraycopyWithDescriptorFallback(
    Thread* self,
    ObjPtr<mirror::ObjectArray<mirror::Object>> dst,
    int32_t dst_pos,
    ObjPtr<mirror::ObjectArray<mirror::Object>> src,
    int32_t src_pos,
    int32_t count,
    ObjPtr<mirror::Class> dst_component_type,
    const char* dst_array_descriptor,
    JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const bool copy_forward =
      src.Ptr() != dst.Ptr() || dst_pos < src_pos || dst_pos - src_pos >= count;
  const auto copy_one = [&](int32_t offset) -> bool {
    ObjPtr<mirror::Object> element = src->GetWithoutChecks(src_pos + offset);
    if (!PFCutArraycopyElementAssignable(dst_component_type, dst_array_descriptor, element)) {
      std::string src_type(mirror::Object::PrettyTypeOf(element));
      std::string dst_type(dst->PrettyTypeOf());
      self->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
                               "source[%d] of type %s cannot be stored in destination array of type %s",
                               src_pos + offset,
                               src_type.c_str(),
                               dst_type.c_str());
      result->SetJ(0);
      return false;
    }
    dst->SetWithoutChecks</*kTransactionActive=*/ false,
                          /*kCheckTransaction=*/ false>(dst_pos + offset, element);
    return true;
  };

  if (copy_forward) {
    for (int32_t i = 0; i < count; ++i) {
      if (!copy_one(i)) {
        return true;
      }
    }
  } else {
    for (int32_t i = count - 1; i >= 0; --i) {
      if (!copy_one(i)) {
        return true;
      }
    }
  }
  result->SetJ(0);
  return true;
}

template <bool is_range>
static inline bool PFCutTrySystemArraycopyIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 5u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/System;") ||
      strcmp(WlSafeName(called_method), "arraycopy") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "VLILII") != 0) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  ObjPtr<mirror::Object> src_object = shadow_frame.GetVRegReference(reg_at(0));
  const int32_t src_pos = static_cast<int32_t>(shadow_frame.GetVReg(reg_at(1)));
  ObjPtr<mirror::Object> dst_object = shadow_frame.GetVRegReference(reg_at(2));
  const int32_t dst_pos = static_cast<int32_t>(shadow_frame.GetVReg(reg_at(3)));
  const int32_t count = static_cast<int32_t>(shadow_frame.GetVReg(reg_at(4)));

  if (UNLIKELY(src_object == nullptr)) {
    ThrowNullPointerException("src == null");
    result->SetJ(0);
    return true;
  }
  if (UNLIKELY(dst_object == nullptr)) {
    ThrowNullPointerException("dst == null");
    result->SetJ(0);
    return true;
  }
  if (UNLIKELY(!src_object->IsArrayInstance())) {
    PFCutThrowArrayStoreExceptionNotAnArray("source", src_object);
    result->SetJ(0);
    return true;
  }
  if (UNLIKELY(!dst_object->IsArrayInstance())) {
    PFCutThrowArrayStoreExceptionNotAnArray("destination", dst_object);
    result->SetJ(0);
    return true;
  }

  ObjPtr<mirror::Array> src_array = src_object->AsArray();
  ObjPtr<mirror::Array> dst_array = dst_object->AsArray();
  if (UNLIKELY(src_pos < 0) || UNLIKELY(dst_pos < 0) || UNLIKELY(count < 0) ||
      UNLIKELY(src_pos > src_array->GetLength() - count) ||
      UNLIKELY(dst_pos > dst_array->GetLength() - count)) {
    self->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d",
                             src_array->GetLength(),
                             src_pos,
                             dst_array->GetLength(),
                             dst_pos,
                             count);
    result->SetJ(0);
    return true;
  }

  ObjPtr<mirror::Class> src_component_type = src_array->GetClass()->GetComponentType();
  ObjPtr<mirror::Class> dst_component_type = dst_array->GetClass()->GetComponentType();
  const Primitive::Type dst_component_primitive_type =
      dst_component_type->GetPrimitiveType();

  if (LIKELY(src_component_type == dst_component_type)) {
    switch (dst_component_primitive_type) {
      case Primitive::kPrimVoid:
        LOG(FATAL) << "Unreachable, cannot have arrays of type void";
        UNREACHABLE();
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
        ObjPtr<mirror::ByteArray>::DownCast(dst_array)->Memmove(
            dst_pos, ObjPtr<mirror::ByteArray>::DownCast(src_array), src_pos, count);
        break;
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
        ObjPtr<mirror::ShortArray>::DownCast(dst_array)->Memmove(
            dst_pos, ObjPtr<mirror::ShortArray>::DownCast(src_array), src_pos, count);
        break;
      case Primitive::kPrimInt:
      case Primitive::kPrimFloat:
        ObjPtr<mirror::IntArray>::DownCast(dst_array)->Memmove(
            dst_pos, ObjPtr<mirror::IntArray>::DownCast(src_array), src_pos, count);
        break;
      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        ObjPtr<mirror::LongArray>::DownCast(dst_array)->Memmove(
            dst_pos, ObjPtr<mirror::LongArray>::DownCast(src_array), src_pos, count);
        break;
      case Primitive::kPrimNot: {
        ObjPtr<mirror::ObjectArray<mirror::Object>> dst_obj_array =
            dst_array->AsObjectArray<mirror::Object>();
        ObjPtr<mirror::ObjectArray<mirror::Object>> src_obj_array =
            src_array->AsObjectArray<mirror::Object>();
        dst_obj_array->AssignableMemmove(dst_pos, src_obj_array, src_pos, count);
        break;
      }
      default:
        LOG(FATAL) << "Unknown array type: " << src_array->PrettyTypeOf();
        UNREACHABLE();
    }
  } else {
    if (UNLIKELY(dst_component_primitive_type != Primitive::kPrimNot ||
                 src_component_type->IsPrimitive())) {
      std::string src_type(src_array->PrettyTypeOf());
      std::string dst_type(dst_array->PrettyTypeOf());
      self->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
                               "Incompatible types: src=%s, dst=%s",
                               src_type.c_str(),
                               dst_type.c_str());
      result->SetJ(0);
      return true;
    }

    ObjPtr<mirror::ObjectArray<mirror::Object>> dst_obj_array =
        dst_array->AsObjectArray<mirror::Object>();
    ObjPtr<mirror::ObjectArray<mirror::Object>> src_obj_array =
        src_array->AsObjectArray<mirror::Object>();
    if (dst_component_type->IsAssignableFrom(src_component_type)) {
      dst_obj_array->AssignableMemcpy(dst_pos, src_obj_array, src_pos, count);
    } else {
      std::string dst_array_desc_storage;
      const char* dst_array_desc = dst_array->GetClass()->GetDescriptor(&dst_array_desc_storage);
      PFCutObjectArraycopyWithDescriptorFallback(self,
                                                 dst_obj_array,
                                                 dst_pos,
                                                 src_obj_array,
                                                 src_pos,
                                                 count,
                                                 dst_component_type,
                                                 dst_array_desc,
                                                 result);
      if (self->IsExceptionPending()) {
        static thread_local int object_arraycopy_fail_count = 0;
        if (object_arraycopy_fail_count < 40) {
          object_arraycopy_fail_count++;
          std::string src_array_desc_storage;
          std::string src_component_desc_storage;
          std::string dst_component_desc_storage;
          const char* src_array_desc =
              src_array->GetClass()->GetDescriptor(&src_array_desc_storage);
          const char* src_component_desc =
              src_component_type->GetDescriptor(&src_component_desc_storage);
          const char* dst_component_desc =
              dst_component_type->GetDescriptor(&dst_component_desc_storage);
          fprintf(stderr,
                  "[PFCUT] System.arraycopy object fallback failed src=%s srcComponent=%s dst=%s dstComponent=%s srcPos=%d dstPos=%d count=%d\n",
                  src_array_desc != nullptr ? src_array_desc : "<null>",
                  src_component_desc != nullptr ? src_component_desc : "<null>",
                  dst_array_desc != nullptr ? dst_array_desc : "<null>",
                  dst_component_desc != nullptr ? dst_component_desc : "<null>",
                  src_pos,
                  dst_pos,
                  count);
          fflush(stderr);
        }
        return true;
      }
    }
  }

  static thread_local int arraycopy_intrinsic_count = 0;
  if (arraycopy_intrinsic_count < 0 /*§650*/) {
    arraycopy_intrinsic_count++;
    std::string src_desc_storage;
    std::string dst_desc_storage;
    const char* src_desc = src_array->GetClass()->GetDescriptor(&src_desc_storage);
    const char* dst_desc = dst_array->GetClass()->GetDescriptor(&dst_desc_storage);
    fprintf(stderr,
            "[PFCUT] System.arraycopy intrinsic src=%s dst=%s srcPos=%d dstPos=%d count=%d\n",
            src_desc != nullptr ? src_desc : "<null>",
            dst_desc != nullptr ? dst_desc : "<null>",
            src_pos,
            dst_pos,
            count);
    fflush(stderr);
  }

  result->SetJ(0);
  return true;
}

template <bool is_range>
static inline bool PFCutTryRuntimeAvailableProcessorsIntrinsic(
    ArtMethod* called_method,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 1u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/Runtime;") ||
      strcmp(WlSafeName(called_method), "availableProcessors") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "I") != 0) {
    return false;
  }

  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  if (shadow_frame.GetVRegReference(receiver_reg) == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetI(0);
    return true;
  }

  static thread_local int available_processors_count = 0;
  if (available_processors_count < 40) {
    available_processors_count++;
    fprintf(stderr,
            "[PFCUT] Runtime.availableProcessors intrinsic -> 4\n");
    fflush(stderr);
  }

  result->SetI(4);
  return true;
}

static inline const char* PFCutThrowableMessage(ObjPtr<mirror::Object> throwable,
                                                std::string* storage)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (throwable == nullptr || storage == nullptr) {
    return "";
  }
  ArtField* detail_message = nullptr;
  for (ObjPtr<mirror::Class> klass = throwable->GetClass();
       klass != nullptr && detail_message == nullptr;
       klass = klass->GetSuperClass()) {
    detail_message = klass->FindInstanceField("detailMessage", "Ljava/lang/String;");
  }
  if (detail_message == nullptr) {
    return "";
  }
  ObjPtr<mirror::Object> message_obj = detail_message->GetObject(throwable);
  if (message_obj == nullptr || !message_obj->GetClass()->DescriptorEquals("Ljava/lang/String;")) {
    return "";
  }
  *storage = message_obj->AsString()->ToModifiedUtf8();
  return storage->c_str();
}

static inline bool PFCutIsThrowableObject(ObjPtr<mirror::Object> object)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (object == nullptr) {
    return false;
  }
  for (ObjPtr<mirror::Class> klass = object->GetClass(); klass != nullptr;
       klass = klass->GetSuperClass()) {
    if (klass->DescriptorEquals("Ljava/lang/Throwable;")) {
      return true;
    }
  }
  return false;
}

static inline void PFCutPrintObjectSummary(ObjPtr<mirror::Object> object, int depth)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (object == nullptr) {
    fprintf(stderr, "null");
    return;
  }

  std::string descriptor_storage;
  const char* descriptor = object->GetClass()->GetDescriptor(&descriptor_storage);
  if (object->GetClass()->DescriptorEquals("Ljava/lang/String;")) {
    std::string text = object->AsString()->ToModifiedUtf8();
    if (text.size() > 160u) {
      text.resize(160u);
      text += "...";
    }
    fprintf(stderr, "String(\"%s\")", text.c_str());
    return;
  }

  if (PFCutIsThrowableObject(object)) {
    std::string message_storage;
    const char* message = PFCutThrowableMessage(object, &message_storage);
    fprintf(stderr,
            "%s(message=\"%s\")",
            descriptor != nullptr ? descriptor : "<null>",
            message != nullptr ? message : "");
    return;
  }

  if (object->IsObjectArray()) {
    ObjPtr<mirror::ObjectArray<mirror::Object>> array =
        object->AsObjectArray<mirror::Object>();
    const int32_t length = array->GetLength();
    fprintf(stderr, "%s[len=%d]", descriptor != nullptr ? descriptor : "ObjectArray", length);
    if (depth < 1) {
      const int32_t limit = std::min<int32_t>(length, 6);
      fprintf(stderr, "{");
      for (int32_t i = 0; i < limit; ++i) {
        if (i != 0) {
          fprintf(stderr, ", ");
        }
        fprintf(stderr, "#%d=", i);
        PFCutPrintObjectSummary(array->GetWithoutChecks(i), depth + 1);
      }
      if (length > limit) {
        fprintf(stderr, ", ...");
      }
      fprintf(stderr, "}");
    }
    return;
  }

  fprintf(stderr, "%s", descriptor != nullptr ? descriptor : "<null>");
}

template <bool is_range>
static inline void PFCutLogKotlinResultFailure(ArtMethod* called_method,
                                              ShadowFrame& shadow_frame,
                                              uint16_t number_of_inputs,
                                              uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                              uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 1u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Lkotlin/ResultKt;") ||
      strcmp(WlSafeName(called_method), "a") != 0) {
    return;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return;
  }

  const uint32_t throwable_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> throwable = shadow_frame.GetVRegReference(throwable_reg);
  if (throwable == nullptr) {
    return;
  }

  static thread_local int kotlin_result_failure_count = 0;
  if (kotlin_result_failure_count >= 80) {
    return;
  }
  kotlin_result_failure_count++;

  std::string descriptor_storage;
  const char* descriptor = throwable->GetClass()->GetDescriptor(&descriptor_storage);
  std::string message_storage;
  const char* message = PFCutThrowableMessage(throwable, &message_storage);
  ArtMethod* caller_method = shadow_frame.GetMethod();
  fprintf(stderr,
          "[PFCUT] Kotlin Result failure caller=%s callee=%s throwable=%s message=%s\n",
          caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
          called_method->PrettyMethod().c_str(),
          descriptor != nullptr ? descriptor : "<null>",
          message);
  fflush(stderr);
}

static inline bool PFCutTryReflectUtilEnsureMemberAccessNoop(ArtMethod* called_method,
                                                             JValue* result,
                                                             uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 4u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Lsun/reflect/misc/ReflectUtil;") ||
      strcmp(WlSafeName(called_method), "ensureMemberAccess") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "VLLLI") != 0) {
    return false;
  }

  static thread_local int ensure_member_access_noop_count = 0;
  if (ensure_member_access_noop_count < 80) {
    ensure_member_access_noop_count++;
    fprintf(stderr,
            "[PFCUT] ReflectUtil.ensureMemberAccess noop %s shorty=%s\n",
            called_method->PrettyMethod().c_str(),
            shorty);
    fflush(stderr);
  }

  // Android app code commonly reaches this from Atomic*FieldUpdater setup.
  // Westlake's class identity/access model is not complete yet, and rejecting
  // private volatile updater fields leaves coroutine runtime updater singletons
  // null before real app code can reach UI creation.
  result->SetJ(0);
  return true;
}

// WESTLAKE §319i: intercept sun.security.jca.ProviderList.removeInvalid() and return the receiver
// (this) unchanged, skipping its eager loadAll() of every configured JCA provider. On this in-process
// ART, loadAll during Providers.<clinit> re-enters Security.addProvider -> Providers.<clinit> ->
// loadAll ~13 levels deep, stalling the ActivityThread main thread so MainActivity.onCreate never runs
// (screen black). core-oj.jar is un-patchable (any dex mod trips a BCP method-resolution SIGBUS,
// §319h), so break the recursion here. Providers then load LAZILY on first crypto use (after the UI).
template <bool is_range>
static inline bool PFCutTryProviderListRemoveInvalidNoop(
    ArtMethod* called_method,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || called_method->GetDeclaringClass() == nullptr) {
    return false;
  }
  std::string ds;
  const char* d = called_method->GetDeclaringClass()->GetDescriptor(&ds);
  if (d == nullptr || strcmp(d, "Lsun/security/jca/ProviderList;") != 0) {
    return false;
  }
  const char* name = WlSafeName(called_method);
  if (name == nullptr || strcmp(name, "removeInvalid") != 0) {
    return false;
  }
  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
  static thread_local int wl_ri_count = 0;
  if (wl_ri_count < 8) {
    wl_ri_count++;
    fprintf(stderr, "[WESTLAKE-JCA] ProviderList.removeInvalid -> this (skip eager loadAll)\n");
    fflush(stderr);
  }
  result->SetL(receiver);
  return true;
}

template <bool is_range>
static inline bool PFCutTryAtomicReferenceArrayIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Ljava/util/concurrent/atomic/AtomicReferenceArray;")) {
    return false;
  }

  const char* name = WlSafeName(called_method);
  if (name == nullptr) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  const bool is_get =
      number_of_inputs == 2u &&
      (strcmp(name, "get") == 0 ||
       strcmp(name, "getPlain") == 0 ||
       strcmp(name, "getOpaque") == 0 ||
       strcmp(name, "getAcquire") == 0);
  const bool is_set =
      number_of_inputs == 3u &&
      (strcmp(name, "set") == 0 ||
       strcmp(name, "lazySet") == 0 ||
       strcmp(name, "setPlain") == 0 ||
       strcmp(name, "setOpaque") == 0 ||
       strcmp(name, "setRelease") == 0);
  const bool is_get_and_set =
      number_of_inputs == 3u && strcmp(name, "getAndSet") == 0;
  const bool is_cas =
      number_of_inputs == 4u &&
      (strcmp(name, "compareAndSet") == 0 ||
       strcmp(name, "weakCompareAndSet") == 0 ||
       strcmp(name, "weakCompareAndSetPlain") == 0 ||
       strcmp(name, "weakCompareAndSetVolatile") == 0 ||
       strcmp(name, "weakCompareAndSetAcquire") == 0 ||
       strcmp(name, "weakCompareAndSetRelease") == 0);
  const bool is_compare_exchange =
      number_of_inputs == 4u &&
      (strcmp(name, "compareAndExchange") == 0 ||
       strcmp(name, "compareAndExchangeAcquire") == 0 ||
       strcmp(name, "compareAndExchangeRelease") == 0);

  if (!is_get && !is_set && !is_get_and_set && !is_cas && !is_compare_exchange) {
    return false;
  }

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(reg_at(0));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }

  ArtField* array_field =
      called_method->GetDeclaringClass()->FindInstanceField("array", "[Ljava/lang/Object;");
  if (array_field == nullptr) {
    return false;
  }

  ObjPtr<mirror::Object> array_obj = array_field->GetObject(receiver);
  if (array_obj == nullptr || !array_obj->IsObjectArray()) {
    ThrowNullPointerException("AtomicReferenceArray.array == null");
    result->SetJ(0);
    return true;
  }

  ObjPtr<mirror::ObjectArray<mirror::Object>> array =
      array_obj->AsObjectArray<mirror::Object>();
  const int32_t index = static_cast<int32_t>(shadow_frame.GetVReg(reg_at(1)));
  const int32_t length = array->GetLength();
  if (index < 0 || index >= length) {
    ThrowArrayIndexOutOfBoundsException(index, length);
    result->SetJ(0);
    return true;
  }

  const MemberOffset element_offset(
      mirror::Array::DataOffset(kHeapReferenceSize).Int32Value() + index * kHeapReferenceSize);
  ObjPtr<mirror::Object> current =
      array->GetFieldObjectVolatile<mirror::Object>(element_offset);
  if (is_get) {
    result->SetL(current);
  } else if (is_set) {
    ObjPtr<mirror::Object> value = shadow_frame.GetVRegReference(reg_at(2));
    array->SetFieldObjectVolatile<false>(element_offset, value);
    result->SetJ(0);
  } else if (is_get_and_set) {
    ObjPtr<mirror::Object> value = shadow_frame.GetVRegReference(reg_at(2));
    do {
      current = array->GetFieldObjectVolatile<mirror::Object>(element_offset);
    } while (!array->CasFieldObject<false>(element_offset,
                                           current,
                                           value,
                                           CASMode::kStrong,
                                           std::memory_order_seq_cst));
    result->SetL(current);
  } else if (is_compare_exchange) {
    ObjPtr<mirror::Object> expected = shadow_frame.GetVRegReference(reg_at(2));
    ObjPtr<mirror::Object> update = shadow_frame.GetVRegReference(reg_at(3));
    do {
      current = array->GetFieldObjectVolatile<mirror::Object>(element_offset);
      if (current.Ptr() != expected.Ptr()) {
        break;
      }
    } while (!array->CasFieldObject<false>(element_offset,
                                           expected,
                                           update,
                                           CASMode::kStrong,
                                           std::memory_order_seq_cst));
    result->SetL(current);
  } else {
    ObjPtr<mirror::Object> expected = shadow_frame.GetVRegReference(reg_at(2));
    ObjPtr<mirror::Object> update = shadow_frame.GetVRegReference(reg_at(3));
    const bool success = current.Ptr() == expected.Ptr();
    if (success) {
      result->SetZ(array->CasFieldObject<false>(element_offset,
                                                expected,
                                                update,
                                                CASMode::kStrong,
                                                std::memory_order_seq_cst));
    } else {
      result->SetZ(false);
    }
  }

  static thread_local int atomic_ref_array_count = 0;
  if (atomic_ref_array_count < 80) {
    atomic_ref_array_count++;
    fprintf(stderr,
            "[PFCUT] AtomicReferenceArray.%s intrinsic index=%d length=%d\n",
            name,
            index,
            length);
    fflush(stderr);
  }
  UNUSED(self);
  return true;
}

template <bool is_range>
static inline bool PFCutTryAtomicReferenceIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Ljava/util/concurrent/atomic/AtomicReference;")) {
    return false;
  }

  const char* name = WlSafeName(called_method);
  if (name == nullptr) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  const bool is_get =
      number_of_inputs == 1u &&
      (strcmp(name, "get") == 0 ||
       strcmp(name, "getPlain") == 0 ||
       strcmp(name, "getOpaque") == 0 ||
       strcmp(name, "getAcquire") == 0);
  const bool is_set =
      number_of_inputs == 2u &&
      (strcmp(name, "set") == 0 ||
       strcmp(name, "lazySet") == 0 ||
       strcmp(name, "setPlain") == 0 ||
       strcmp(name, "setOpaque") == 0 ||
       strcmp(name, "setRelease") == 0);
  const bool is_get_and_set =
      number_of_inputs == 2u && strcmp(name, "getAndSet") == 0;
  const bool is_cas =
      number_of_inputs == 3u &&
      (strcmp(name, "compareAndSet") == 0 ||
       strcmp(name, "weakCompareAndSet") == 0 ||
       strcmp(name, "weakCompareAndSetPlain") == 0 ||
       strcmp(name, "weakCompareAndSetVolatile") == 0 ||
       strcmp(name, "weakCompareAndSetAcquire") == 0 ||
       strcmp(name, "weakCompareAndSetRelease") == 0);
  const bool is_compare_exchange =
      number_of_inputs == 3u &&
      (strcmp(name, "compareAndExchange") == 0 ||
       strcmp(name, "compareAndExchangeAcquire") == 0 ||
       strcmp(name, "compareAndExchangeRelease") == 0);

  if (!is_get && !is_set && !is_get_and_set && !is_cas && !is_compare_exchange) {
    return false;
  }

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(reg_at(0));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }

  ArtField* value_field =
      called_method->GetDeclaringClass()->FindInstanceField("value", "Ljava/lang/Object;");
  if (value_field == nullptr) {
    return false;
  }

  const MemberOffset value_offset = value_field->GetOffset();
  ObjPtr<mirror::Object> current =
      receiver->GetFieldObjectVolatile<mirror::Object>(value_offset);
  if (is_get) {
    result->SetL(current);
  } else if (is_set) {
    ObjPtr<mirror::Object> value = shadow_frame.GetVRegReference(reg_at(1));
    receiver->SetFieldObjectVolatile<false>(value_offset, value);
    result->SetJ(0);
  } else if (is_get_and_set) {
    ObjPtr<mirror::Object> value = shadow_frame.GetVRegReference(reg_at(1));
    do {
      current = receiver->GetFieldObjectVolatile<mirror::Object>(value_offset);
    } while (!receiver->CasFieldObject<false>(value_offset,
                                              current,
                                              value,
                                              CASMode::kStrong,
                                              std::memory_order_seq_cst));
    result->SetL(current);
  } else if (is_compare_exchange) {
    ObjPtr<mirror::Object> expected = shadow_frame.GetVRegReference(reg_at(1));
    ObjPtr<mirror::Object> update = shadow_frame.GetVRegReference(reg_at(2));
    do {
      current = receiver->GetFieldObjectVolatile<mirror::Object>(value_offset);
      if (current.Ptr() != expected.Ptr()) {
        break;
      }
    } while (!receiver->CasFieldObject<false>(value_offset,
                                              expected,
                                              update,
                                              CASMode::kStrong,
                                              std::memory_order_seq_cst));
    result->SetL(current);
  } else {
    ObjPtr<mirror::Object> expected = shadow_frame.GetVRegReference(reg_at(1));
    ObjPtr<mirror::Object> update = shadow_frame.GetVRegReference(reg_at(2));
    const bool success = current.Ptr() == expected.Ptr();
    if (success) {
      result->SetZ(receiver->CasFieldObject<false>(value_offset,
                                                   expected,
                                                   update,
                                                   CASMode::kStrong,
                                                   std::memory_order_seq_cst));
    } else {
      result->SetZ(false);
    }
  }

  static thread_local int atomic_ref_count = 0;
  if (atomic_ref_count < 120) {
    atomic_ref_count++;
    fprintf(stderr,
            "[PFCUT] AtomicReference.%s intrinsic current=%p\n",
            name,
            current.Ptr());
    fflush(stderr);
  }
  UNUSED(self);
  return true;
}

template <bool is_range>
static inline bool PFCutTryAtomicIntegerOrBooleanIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  const bool is_integer =
      called_method->GetDeclaringClass()->DescriptorEquals(
          "Ljava/util/concurrent/atomic/AtomicInteger;");
  const bool is_boolean =
      called_method->GetDeclaringClass()->DescriptorEquals(
          "Ljava/util/concurrent/atomic/AtomicBoolean;");
  if (!is_integer && !is_boolean) {
    return false;
  }

  const char* name = WlSafeName(called_method);
  if (name == nullptr) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  const bool is_get =
      number_of_inputs == 1u &&
      (strcmp(name, "get") == 0 ||
       strcmp(name, "getPlain") == 0 ||
       strcmp(name, "getOpaque") == 0 ||
       strcmp(name, "getAcquire") == 0);
  const bool is_set =
      number_of_inputs == 2u &&
      (strcmp(name, "set") == 0 ||
       strcmp(name, "lazySet") == 0 ||
       strcmp(name, "setPlain") == 0 ||
       strcmp(name, "setOpaque") == 0 ||
       strcmp(name, "setRelease") == 0);
  const bool is_get_and_set =
      number_of_inputs == 2u && strcmp(name, "getAndSet") == 0;
  const bool is_cas =
      number_of_inputs == 3u &&
      (strcmp(name, "compareAndSet") == 0 ||
       strcmp(name, "weakCompareAndSet") == 0 ||
       strcmp(name, "weakCompareAndSetPlain") == 0 ||
       strcmp(name, "weakCompareAndSetVolatile") == 0 ||
       strcmp(name, "weakCompareAndSetAcquire") == 0 ||
       strcmp(name, "weakCompareAndSetRelease") == 0);
  const bool is_compare_exchange =
      number_of_inputs == 3u &&
      (strcmp(name, "compareAndExchange") == 0 ||
       strcmp(name, "compareAndExchangeAcquire") == 0 ||
       strcmp(name, "compareAndExchangeRelease") == 0);
  const bool is_get_and_add =
      is_integer &&
      ((number_of_inputs == 1u &&
        (strcmp(name, "getAndIncrement") == 0 ||
         strcmp(name, "getAndDecrement") == 0)) ||
       (number_of_inputs == 2u && strcmp(name, "getAndAdd") == 0));
  const bool is_add_and_get =
      is_integer &&
      ((number_of_inputs == 1u &&
        (strcmp(name, "incrementAndGet") == 0 ||
         strcmp(name, "decrementAndGet") == 0)) ||
       (number_of_inputs == 2u && strcmp(name, "addAndGet") == 0));

  if (!is_get &&
      !is_set &&
      !is_get_and_set &&
      !is_cas &&
      !is_compare_exchange &&
      !is_get_and_add &&
      !is_add_and_get) {
    return false;
  }

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(reg_at(0));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }

  ArtField* value_field =
      called_method->GetDeclaringClass()->FindInstanceField("value", "I");
  if (value_field == nullptr) {
    return false;
  }

  const MemberOffset value_offset = value_field->GetOffset();
  int32_t current = receiver->GetField32Volatile(value_offset);
  const auto normalize = [&](int32_t value) -> int32_t {
    return is_boolean ? (value != 0 ? 1 : 0) : value;
  };

  if (is_get) {
    if (is_boolean) {
      result->SetZ(current != 0);
    } else {
      result->SetI(current);
    }
  } else if (is_set) {
    const int32_t value = normalize(static_cast<int32_t>(shadow_frame.GetVReg(reg_at(1))));
    receiver->SetField32Volatile<false>(value_offset, value);
    result->SetJ(0);
  } else if (is_get_and_set) {
    const int32_t value = normalize(static_cast<int32_t>(shadow_frame.GetVReg(reg_at(1))));
    do {
      current = receiver->GetField32Volatile(value_offset);
    } while (!receiver->CasField32<false>(value_offset,
                                          current,
                                          value,
                                          CASMode::kStrong,
                                          std::memory_order_seq_cst));
    if (is_boolean) {
      result->SetZ(current != 0);
    } else {
      result->SetI(current);
    }
  } else if (is_compare_exchange) {
    const int32_t expected =
        normalize(static_cast<int32_t>(shadow_frame.GetVReg(reg_at(1))));
    const int32_t update =
        normalize(static_cast<int32_t>(shadow_frame.GetVReg(reg_at(2))));
    do {
      current = receiver->GetField32Volatile(value_offset);
      if (current != expected) {
        break;
      }
    } while (!receiver->CasField32<false>(value_offset,
                                          expected,
                                          update,
                                          CASMode::kStrong,
                                          std::memory_order_seq_cst));
    if (is_boolean) {
      result->SetZ(current != 0);
    } else {
      result->SetI(current);
    }
  } else if (is_cas) {
    const int32_t expected =
        normalize(static_cast<int32_t>(shadow_frame.GetVReg(reg_at(1))));
    const int32_t update =
        normalize(static_cast<int32_t>(shadow_frame.GetVReg(reg_at(2))));
    const bool success = current == expected;
    if (success) {
      result->SetZ(receiver->CasField32<false>(value_offset,
                                               expected,
                                               update,
                                               CASMode::kStrong,
                                               std::memory_order_seq_cst));
    } else {
      result->SetZ(false);
    }
  } else {
    int32_t delta = 0;
    if (strcmp(name, "getAndIncrement") == 0 || strcmp(name, "incrementAndGet") == 0) {
      delta = 1;
    } else if (strcmp(name, "getAndDecrement") == 0 || strcmp(name, "decrementAndGet") == 0) {
      delta = -1;
    } else {
      delta = static_cast<int32_t>(shadow_frame.GetVReg(reg_at(1)));
    }
    do {
      current = receiver->GetField32Volatile(value_offset);
    } while (!receiver->CasField32<false>(value_offset,
                                          current,
                                          current + delta,
                                          CASMode::kStrong,
                                          std::memory_order_seq_cst));
    const int32_t updated = current + delta;
    result->SetI(is_get_and_add ? current : updated);
  }

  static thread_local int atomic_int_bool_count = 0;
  if (atomic_int_bool_count < 160) {
    atomic_int_bool_count++;
    fprintf(stderr,
            "[PFCUT] %s.%s intrinsic current=%d\n",
            is_boolean ? "AtomicBoolean" : "AtomicInteger",
            name,
            current);
    fflush(stderr);
  }
  UNUSED(self);
  return true;
}

template <bool is_range>
static inline bool PFCutTryAtomicLongIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Ljava/util/concurrent/atomic/AtomicLong;")) {
    return false;
  }

  const char* name = WlSafeName(called_method);
  if (name == nullptr) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  const bool is_get =
      number_of_inputs == 1u &&
      (strcmp(name, "get") == 0 ||
       strcmp(name, "getPlain") == 0 ||
       strcmp(name, "getOpaque") == 0 ||
       strcmp(name, "getAcquire") == 0);
  const bool is_set =
      number_of_inputs == 3u &&
      (strcmp(name, "set") == 0 ||
       strcmp(name, "lazySet") == 0 ||
       strcmp(name, "setPlain") == 0 ||
       strcmp(name, "setOpaque") == 0 ||
       strcmp(name, "setRelease") == 0);
  const bool is_get_and_set =
      number_of_inputs == 3u && strcmp(name, "getAndSet") == 0;
  const bool is_cas =
      number_of_inputs == 5u &&
      (strcmp(name, "compareAndSet") == 0 ||
       strcmp(name, "weakCompareAndSet") == 0 ||
       strcmp(name, "weakCompareAndSetPlain") == 0 ||
       strcmp(name, "weakCompareAndSetVolatile") == 0 ||
       strcmp(name, "weakCompareAndSetAcquire") == 0 ||
       strcmp(name, "weakCompareAndSetRelease") == 0);
  const bool is_compare_exchange =
      number_of_inputs == 5u &&
      (strcmp(name, "compareAndExchange") == 0 ||
       strcmp(name, "compareAndExchangeAcquire") == 0 ||
       strcmp(name, "compareAndExchangeRelease") == 0);
  const bool is_get_and_add =
      (number_of_inputs == 1u &&
       (strcmp(name, "getAndIncrement") == 0 ||
        strcmp(name, "getAndDecrement") == 0)) ||
      (number_of_inputs == 3u && strcmp(name, "getAndAdd") == 0);
  const bool is_add_and_get =
      (number_of_inputs == 1u &&
       (strcmp(name, "incrementAndGet") == 0 ||
        strcmp(name, "decrementAndGet") == 0)) ||
      (number_of_inputs == 3u && strcmp(name, "addAndGet") == 0);

  if (!is_get &&
      !is_set &&
      !is_get_and_set &&
      !is_cas &&
      !is_compare_exchange &&
      !is_get_and_add &&
      !is_add_and_get) {
    return false;
  }

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(reg_at(0));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }

  ArtField* value_field =
      called_method->GetDeclaringClass()->FindInstanceField("value", "J");
  if (value_field == nullptr) {
    return false;
  }

  const MemberOffset value_offset = value_field->GetOffset();
  int64_t current = receiver->GetField64Volatile(value_offset);
  if (is_get) {
    result->SetJ(current);
  } else if (is_set) {
    const int64_t value = shadow_frame.GetVRegLong(reg_at(1));
    receiver->SetField64Volatile<false>(value_offset, value);
    result->SetJ(0);
  } else if (is_get_and_set) {
    const int64_t value = shadow_frame.GetVRegLong(reg_at(1));
    do {
      current = receiver->GetField64Volatile(value_offset);
    } while (!receiver->CasFieldStrongSequentiallyConsistent64<false>(value_offset,
                                                                      current,
                                                                      value));
    result->SetJ(current);
  } else if (is_compare_exchange) {
    const int64_t expected = shadow_frame.GetVRegLong(reg_at(1));
    const int64_t update = shadow_frame.GetVRegLong(reg_at(3));
    do {
      current = receiver->GetField64Volatile(value_offset);
      if (current != expected) {
        break;
      }
    } while (!receiver->CasFieldStrongSequentiallyConsistent64<false>(value_offset,
                                                                      expected,
                                                                      update));
    result->SetJ(current);
  } else if (is_cas) {
    const int64_t expected = shadow_frame.GetVRegLong(reg_at(1));
    const int64_t update = shadow_frame.GetVRegLong(reg_at(3));
    const bool success = current == expected;
    if (success) {
      result->SetZ(receiver->CasFieldStrongSequentiallyConsistent64<false>(value_offset,
                                                                           expected,
                                                                           update));
    } else {
      result->SetZ(false);
    }
  } else {
    int64_t delta = 0;
    if (strcmp(name, "getAndIncrement") == 0 || strcmp(name, "incrementAndGet") == 0) {
      delta = 1;
    } else if (strcmp(name, "getAndDecrement") == 0 || strcmp(name, "decrementAndGet") == 0) {
      delta = -1;
    } else {
      delta = shadow_frame.GetVRegLong(reg_at(1));
    }
    do {
      current = receiver->GetField64Volatile(value_offset);
    } while (!receiver->CasFieldStrongSequentiallyConsistent64<false>(value_offset,
                                                                      current,
                                                                      current + delta));
    const int64_t updated = current + delta;
    result->SetJ(is_get_and_add ? current : updated);
  }

  static thread_local int atomic_long_count = 0;
  if (atomic_long_count < 120) {
    atomic_long_count++;
    fprintf(stderr,
            "[PFCUT] AtomicLong.%s intrinsic current=%lld\n",
            name,
            static_cast<long long>(current));
    fflush(stderr);
  }
  UNUSED(self);
  return true;
}

template <bool is_range>
static inline bool PFCutTryUnixFileSystemListIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 2u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/io/UnixFileSystem;") ||
      strcmp(WlSafeName(called_method), "list") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  ObjPtr<mirror::Object> file = shadow_frame.GetVRegReference(reg_at(1));
  if (file == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }

  std::string path;
  ArtField* path_field = file->GetClass()->FindInstanceField("path", "Ljava/lang/String;");
  if (path_field != nullptr) {
    ObjPtr<mirror::Object> path_obj = path_field->GetObject(file);
    if (path_obj != nullptr && path_obj->IsString()) {
      path = path_obj->AsString()->ToModifiedUtf8();
    }
  }

  std::vector<std::string> dynamic_entries;
  if (path.find("/sys/devices/system/cpu") != std::string::npos ||
      path == "/sys/devices/system/cpu") {
    dynamic_entries = {"cpu0", "cpu1", "cpu2", "cpu3", "cpu4", "cpu5", "cpu6", "cpu7"};
  } else if (!path.empty()) {
    DIR* dir = opendir(path.c_str());
    if (dir != nullptr) {
      while (dirent* entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
          continue;
        }
        dynamic_entries.emplace_back(entry->d_name);
      }
      closedir(dir);
      std::sort(dynamic_entries.begin(), dynamic_entries.end());
    }
  }

  StackHandleScope<1> hs(self);
  Handle<mirror::ObjectArray<mirror::String>> h_array(hs.NewHandle(
      mirror::ObjectArray<mirror::String>::Alloc(
          self,
          GetClassRoot<mirror::ObjectArray<mirror::String>>(),
          static_cast<int32_t>(dynamic_entries.size()))));
  if (h_array == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  for (int32_t i = 0; i < static_cast<int32_t>(dynamic_entries.size()); ++i) {
    ObjPtr<mirror::String> entry =
        mirror::String::AllocFromModifiedUtf8(self, dynamic_entries[i].c_str());
    if (entry == nullptr) {
      result->SetL(nullptr);
      return true;
    }
    h_array->SetWithoutChecks</*kTransactionActive=*/ false,
                               /*kCheckTransaction=*/ false>(i, entry);
  }

  static thread_local int unixfs_list_count = 0;
  if (unixfs_list_count < 80) {
    unixfs_list_count++;
    fprintf(stderr,
            "[PFCUT] UnixFileSystem.list intrinsic path=%s entries=%zu\n",
            path.empty() ? "<unknown>" : path.c_str(),
            dynamic_entries.size());
    fflush(stderr);
  }
  result->SetL(h_array.Get());
  return true;
}

template <bool is_range>
static inline bool PFCutTryUnixFileSystemGetBooleanAttributesIntrinsic(
    ArtMethod* called_method,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 2u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/io/UnixFileSystem;") ||
      strcmp(WlSafeName(called_method), "getBooleanAttributes") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "IL") != 0) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  ObjPtr<mirror::Object> file = shadow_frame.GetVRegReference(reg_at(1));
  if (file == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetI(0);
    return true;
  }

  std::string path;
  ArtField* path_field = file->GetClass()->FindInstanceField("path", "Ljava/lang/String;");
  if (path_field != nullptr) {
    ObjPtr<mirror::Object> path_obj = path_field->GetObject(file);
    if (path_obj != nullptr && path_obj->IsString()) {
      path = path_obj->AsString()->ToModifiedUtf8();
    }
  }

  int32_t attrs = 0;
  struct stat sb;
  if (!path.empty() && stat(path.c_str(), &sb) == 0) {
    static constexpr int32_t kExists = 0x01;
    static constexpr int32_t kRegular = 0x02;
    static constexpr int32_t kDirectory = 0x04;
    static constexpr int32_t kHidden = 0x08;
    attrs = kExists;
    if (S_ISREG(sb.st_mode)) {
      attrs |= kRegular;
    }
    if (S_ISDIR(sb.st_mode)) {
      attrs |= kDirectory;
    }
    const size_t slash = path.find_last_of('/');
    const size_t base = slash == std::string::npos ? 0u : slash + 1u;
    if (base < path.size() && path[base] == '.' && base + 1u < path.size()) {
      attrs |= kHidden;
    }
  }

  static thread_local int unixfs_attrs_count = 0;
  if (unixfs_attrs_count < 120) {
    unixfs_attrs_count++;
    fprintf(stderr,
            "[PFCUT] UnixFileSystem.getBooleanAttributes intrinsic path=%s attrs=0x%x\n",
            path.empty() ? "<unknown>" : path.c_str(),
            attrs);
    fflush(stderr);
  }
  result->SetI(attrs);
  return true;
}

template <bool is_range>
static inline bool PFCutTryUnixFileSystemGetLengthIntrinsic(
    ArtMethod* called_method,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 2u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/io/UnixFileSystem;") ||
      strcmp(WlSafeName(called_method), "getLength") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "JL") != 0) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  ObjPtr<mirror::Object> file = shadow_frame.GetVRegReference(reg_at(1));
  if (file == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }

  std::string path;
  ArtField* path_field = file->GetClass()->FindInstanceField("path", "Ljava/lang/String;");
  if (path_field != nullptr) {
    ObjPtr<mirror::Object> path_obj = path_field->GetObject(file);
    if (path_obj != nullptr && path_obj->IsString()) {
      path = path_obj->AsString()->ToModifiedUtf8();
    }
  }

  int64_t length = 0;
  struct stat sb;
  if (!path.empty() && stat(path.c_str(), &sb) == 0 && S_ISREG(sb.st_mode)) {
    length = static_cast<int64_t>(sb.st_size);
  }

  static thread_local int unixfs_length_count = 0;
  if (unixfs_length_count < 120) {
    unixfs_length_count++;
    fprintf(stderr,
            "[PFCUT] UnixFileSystem.getLength intrinsic path=%s length=%lld\n",
            path.empty() ? "<unknown>" : path.c_str(),
            static_cast<long long>(length));
    fflush(stderr);
  }
  result->SetJ(length);
  return true;
}

template <bool is_range>
static inline bool PFCutTryUnixFileSystemCanonicalizeIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 2u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/io/UnixFileSystem;") ||
      (strcmp(WlSafeName(called_method), "canonicalize0") != 0 &&
       strcmp(WlSafeName(called_method), "canonicalize") != 0)) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  const uint32_t path_reg = is_range ? (vregC + 1u) : arg[1];
  ObjPtr<mirror::Object> path_obj = shadow_frame.GetVRegReference(path_reg);
  if (path_obj == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetL(nullptr);
    return true;
  }
  if (!path_obj->IsString()) {
    return false;
  }

  const std::string path = path_obj->AsString()->ToModifiedUtf8();
  char resolved[PATH_MAX];
  ObjPtr<mirror::String> out = path_obj->AsString();
  if (!path.empty() && realpath(path.c_str(), resolved) != nullptr) {
    ObjPtr<mirror::String> resolved_string =
        mirror::String::AllocFromModifiedUtf8(self, resolved);
    if (resolved_string != nullptr) {
      out = resolved_string;
    }
  }

  static thread_local int canonicalize_count = 0;
  if (canonicalize_count < 80) {
    canonicalize_count++;
    fprintf(stderr,
            "[PFCUT] UnixFileSystem.%s intrinsic path=%s result=%s\n",
            WlSafeName(called_method),
            path.c_str(),
            out != nullptr ? out->ToModifiedUtf8().c_str() : "<null>");
    fflush(stderr);
  }
  result->SetL(out);
  return true;
}

template <bool is_range>
static inline bool PFCutTryLinuxOpenIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 4u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Llibcore/io/Linux;") ||
      strcmp(WlSafeName(called_method), "open") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LLII") != 0) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(reg_at(0));
  ObjPtr<mirror::Object> path_obj = shadow_frame.GetVRegReference(reg_at(1));
  if (receiver == nullptr || path_obj == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetL(nullptr);
    return true;
  }
  if (!path_obj->IsString()) {
    return false;
  }

  const std::string path = path_obj->AsString()->ToModifiedUtf8();
  const int32_t flags = shadow_frame.GetVReg(reg_at(2));
  const int32_t mode = shadow_frame.GetVReg(reg_at(3));
  int fd = -1;
  int saved_errno = EINVAL;
  if (!path.empty()) {
    fd = ::open(path.c_str(), static_cast<int>(flags), static_cast<mode_t>(mode));
    saved_errno = errno;
  }

  // WESTLAKE §293h (2026-07-23): SystemFonts hard-codes /system/etc/fonts.xml, but an OH app
  // sandbox does not mount /system/etc. MEASURED from inside the ability process:
  //   etc/fonts.xml=0  android/etc/fonts.xml=0  ttf=1  fontsdir=1
  // i.e. the .ttf files ARE reachable but the config is not. With no config the system font map
  // comes back empty, so Typeface.DEFAULT stays null and Typeface.create(family,style) NPEs on
  // family.mStyle inside loadPreinstalledSystemFontMap -> ensureBindApplication FAILS ->
  // mInitialApplication null -> Application.getResources() NPE -> the app dies.
  // Retry from a sandbox-readable copy. Only fires when the stock open already FAILED, so the
  // appspawn-x child (which can read /system/etc) is completely unaffected.
  if (fd < 0 && path.find("fonts.xml") != std::string::npos) {
    static const char* kFontCfgFallback[] = {
      "/data/storage/el1/base/asx/fonts.xml",  // ability process (app sandbox)
      "/data/local/tmp/asx/fonts.xml",         // appspawn-x staging
    };
    for (const char* cand : kFontCfgFallback) {
      fd = ::open(cand, static_cast<int>(flags), static_cast<mode_t>(mode));
      if (fd >= 0) {
        fprintf(stderr, "[WESTLAKE] fonts.xml redirect %s -> %s\n", path.c_str(), cand);
        fflush(stderr);
        break;
      }
    }
    if (fd < 0) { saved_errno = errno; }
  }

  bool used_dev_null = false;
  const int access_mode = flags & O_ACCMODE;
  const bool wants_write = access_mode == O_WRONLY ||
                           access_mode == O_RDWR ||
                           (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0;
  const bool optional_platform_probe =
      !wants_write &&
      saved_errno == ENOENT &&
      path == "/apex/com.android.art/lib/logging.properties";
  if (fd < 0 && optional_platform_probe) {
    fd = ::open("/dev/null", O_RDONLY);
    used_dev_null = fd >= 0;
  }
  if (fd < 0) {
    self->ThrowNewExceptionF("Ljava/io/FileNotFoundException;",
                             "open failed for %s: errno %d",
                             path.empty() ? "<empty>" : path.c_str(),
                             saved_errno);
    result->SetL(nullptr);
    return true;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<6> hs(self);
  Handle<mirror::Class> h_fd_class(
      hs.NewHandle(class_linker->FindSystemClass(self, "Ljava/io/FileDescriptor;")));
  if (h_fd_class == nullptr) {
    ::close(fd);
    result->SetL(nullptr);
    return true;
  }
  if (!class_linker->EnsureInitialized(self, h_fd_class, true, true)) {
    ::close(fd);
    result->SetL(nullptr);
    return true;
  }

  Handle<mirror::Object> h_fd(hs.NewHandle(h_fd_class->AllocObject(self)));
  if (h_fd == nullptr) {
    ::close(fd);
    result->SetL(nullptr);
    return true;
  }
  ArtField* descriptor_field = h_fd_class->FindInstanceField("descriptor", "I");
  if (descriptor_field == nullptr) {
    ::close(fd);
    self->ThrowNewException("Ljava/lang/InternalError;",
                            "java.io.FileDescriptor.descriptor missing");
    result->SetL(nullptr);
    return true;
  }
  descriptor_field->SetInt<false>(h_fd.Get(), fd);
  ArtField* owner_id_field = h_fd_class->FindInstanceField("ownerId", "J");
  if (owner_id_field != nullptr) {
    owner_id_field->SetLong<false>(h_fd.Get(), 0);
  }
  ArtField* append_field = h_fd_class->FindInstanceField("append", "Z");
  if (append_field != nullptr) {
    append_field->SetBoolean<false>(h_fd.Get(), (flags & O_APPEND) != 0 ? 1u : 0u);
  }
  ArtField* release_lock_field =
      h_fd_class->FindInstanceField("releaseLock", "Ljava/lang/Object;");
  if (release_lock_field != nullptr) {
    Handle<mirror::Class> h_object_class(
        hs.NewHandle(class_linker->FindSystemClass(self, "Ljava/lang/Object;")));
    if (h_object_class != nullptr) {
      Handle<mirror::Object> h_release_lock(hs.NewHandle(h_object_class->AllocObject(self)));
      if (h_release_lock != nullptr) {
        release_lock_field->SetObject<false>(h_fd.Get(), h_release_lock.Get());
      }
    }
  }

  static thread_local int linux_open_count = 0;
  if (linux_open_count < 160) {
    linux_open_count++;
    fprintf(stderr,
            "[PFCUT] Linux.open intrinsic path=%s flags=0x%x mode=0%o fd=%d%s errno=%d\n",
            path.empty() ? "<empty>" : path.c_str(),
            flags,
            mode,
            fd,
            used_dev_null ? " devnull" : "",
            used_dev_null ? saved_errno : 0);
    fflush(stderr);
  }
  result->SetL(h_fd.Get());
  return true;
}

template <bool is_range>
static inline bool PFCutTryClassGetDeclaredFieldIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 2u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/Class;") ||
      strcmp(WlSafeName(called_method), "getDeclaredField") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  const uint32_t class_reg = is_range ? vregC : arg[0];
  const uint32_t name_reg = is_range ? vregC + 1u : arg[1];
  ObjPtr<mirror::Object> class_obj = shadow_frame.GetVRegReference(class_reg);
  ObjPtr<mirror::Object> name_obj = shadow_frame.GetVRegReference(name_reg);
  if (class_obj == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetL(nullptr);
    return true;
  }
  if (name_obj == nullptr) {
    ThrowNullPointerException("name == null");
    result->SetL(nullptr);
    return true;
  }
  if (!class_obj->IsClass() || !name_obj->IsString()) {
    return false;
  }

  StackHandleScope<3> hs(self);
  Handle<mirror::Class> h_class(hs.NewHandle(class_obj->AsClass()));
  Handle<mirror::String> h_name(hs.NewHandle(name_obj->AsString()));
  if (h_class->IsObsoleteObject()) {
    self->ThrowNewException("Ljava/lang/RuntimeException;", "Obsolete Object!");
    result->SetL(nullptr);
    return true;
  }

  ArtField* found = nullptr;
  for (ArtField& field : h_class->GetIFields()) {
    if (h_name->Equals(field.GetName())) {
      found = &field;
      break;
    }
  }
  if (found == nullptr) {
    for (ArtField& field : h_class->GetSFields()) {
      if (h_name->Equals(field.GetName())) {
        found = &field;
        break;
      }
    }
  }

  if (found == nullptr) {
    std::string field_name = h_name->ToModifiedUtf8();
    std::string class_name = h_class->PrettyDescriptor();
    static thread_local int class_get_declared_field_miss_count = 0;
    if (class_get_declared_field_miss_count < 120) {
      class_get_declared_field_miss_count++;
      fprintf(stderr,
              "[PFCUT] Class.getDeclaredField intrinsic miss class=%s name=%s\n",
              class_name.c_str(),
              field_name.c_str());
      fflush(stderr);
    }
    self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldException;",
                             "%s.%s",
                             class_name.c_str(),
                             field_name.c_str());
    result->SetL(nullptr);
    return true;
  }

  Handle<mirror::Field> h_field(
      hs.NewHandle(mirror::Field::CreateFromArtField(self, found, true)));
  if (h_field == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  static thread_local int class_get_declared_field_hit_count = 0;
  if (class_get_declared_field_hit_count < 120) {
    class_get_declared_field_hit_count++;
    fprintf(stderr,
            "[PFCUT] Class.getDeclaredField intrinsic hit class=%s name=%s\n",
            h_class->PrettyDescriptor().c_str(),
            h_name->ToModifiedUtf8().c_str());
    fflush(stderr);
  }
  result->SetL(h_field.Get());
  return true;
}

template <bool is_range>
static inline bool PFCutTryClassNewInstanceIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 1u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/Class;") ||
      strcmp(WlSafeName(called_method), "newInstance") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "L") != 0) {
    return false;
  }

  const uint32_t class_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> class_obj = shadow_frame.GetVRegReference(class_reg);
  if (class_obj == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetL(nullptr);
    return true;
  }
  if (!class_obj->IsClass()) {
    return false;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> h_class(hs.NewHandle(class_obj->AsClass()));
  if (h_class->IsObsoleteObject()) {
    self->ThrowNewException("Ljava/lang/RuntimeException;", "Obsolete Object!");
    result->SetL(nullptr);
    return true;
  }
  if (h_class->GetPrimitiveType() != 0 ||
      h_class->IsInterface() ||
      h_class->IsArrayClass() ||
      h_class->IsAbstract()) {
    self->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
                             "%s cannot be instantiated",
                             h_class->PrettyClass().c_str());
    result->SetL(nullptr);
    return true;
  }

  if (h_class->IsStringClass()) {
    gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
    ObjPtr<mirror::Object> string_obj = mirror::String::AllocEmptyString(self, allocator_type);
    result->SetL(string_obj);
    return true;
  }

  ArtMethod* constructor = h_class->GetDeclaredConstructor(
      self, ScopedNullHandle<mirror::ObjectArray<mirror::Class>>(), kRuntimePointerSize);
  if (constructor == nullptr || self->IsExceptionPending()) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    self->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
                             "%s has no zero argument constructor",
                             h_class->PrettyClass().c_str());
    result->SetL(nullptr);
    return true;
  }

  if (!h_class->IsVisiblyInitialized() &&
      !class_linker->EnsureInitialized(self, h_class, true, true)) {
    result->SetL(nullptr);
    return true;
  }

  Handle<mirror::Object> h_receiver(hs.NewHandle(h_class->AllocObject(self)));
  if (h_receiver == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  JValue constructor_result;
  uint32_t constructor_args[1] = {
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h_receiver.Get()))};
  constructor->Invoke(self, constructor_args, sizeof(constructor_args), &constructor_result, "V");
  if (self->IsExceptionPending()) {
    result->SetL(nullptr);
    return true;
  }

  static thread_local int class_new_instance_count = 0;
  if (class_new_instance_count < 80) {
    class_new_instance_count++;
    fprintf(stderr,
            "[PFCUT] Class.newInstance intrinsic class=%s ctor=%s\n",
            h_class->PrettyClass().c_str(),
            constructor->PrettyMethod().c_str());
    fflush(stderr);
  }
  result->SetL(h_receiver.Get());
  return true;
}

static inline bool PFCutTryMethodHandlesLookupIntrinsic(ArtMethod* called_method,
                                                        Thread* self,
                                                        ShadowFrame& shadow_frame,
                                                        JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/invoke/MethodHandles;")) {
    return false;
  }

  const char* method_name = WlSafeName(called_method);
  if (method_name == nullptr || strcmp(method_name, "lookup") != 0) {
    return false;
  }

  ArtMethod* caller_method = shadow_frame.GetMethod();
  if (caller_method == nullptr || caller_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  StackHandleScope<1> hs(self);
  Handle<mirror::Class> lookup_class(hs.NewHandle(caller_method->GetDeclaringClass()));
  ObjPtr<mirror::MethodHandlesLookup> lookup =
      mirror::MethodHandlesLookup::Create(self, lookup_class);
  if (lookup == nullptr) {
    return true;
  }

  static thread_local int method_handles_lookup_count = 0;
  if (method_handles_lookup_count < 80) {
    method_handles_lookup_count++;
    fprintf(stderr,
            "[PFCUT] MethodHandles.lookup intrinsic caller=%s\n",
            caller_method->PrettyMethod().c_str());
    fflush(stderr);
  }
  result->SetL(lookup);
  return true;
}

static void PFCutAppendPathList(std::vector<std::string>* out, const char* path_list)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (path_list == nullptr || path_list[0] == '\0') {
    return;
  }
  const char* start = path_list;
  for (const char* p = path_list; ; ++p) {
    if (*p == ':' || *p == '\0') {
      if (p > start) {
        out->emplace_back(start, p - start);
      }
      if (*p == '\0') {
        break;
      }
      start = p + 1;
    }
  }
}

template <bool is_range>
static inline bool PFCutTryDexPathListFindLibraryIntrinsic(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ldalvik/system/DexPathList;")) {
    return false;
  }

  const char* method_name = WlSafeName(called_method);
  if (method_name == nullptr || strcmp(method_name, "findLibrary") != 0 ||
      number_of_inputs < 2u) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };
  ObjPtr<mirror::Object> name_obj = shadow_frame.GetVRegReference(reg_at(1));
  if (name_obj == nullptr || !name_obj->IsString()) {
    result->SetL(nullptr);
    return true;
  }

  std::string library_name = name_obj->AsString()->ToModifiedUtf8();
  std::string file_name = library_name;
  if (!(file_name.size() > 6u &&
        file_name.compare(0, 3, "lib") == 0 &&
        file_name.compare(file_name.size() - 3u, 3, ".so") == 0)) {
    file_name = "lib" + library_name + ".so";
  }

  std::vector<std::string> dirs;
  PFCutAppendPathList(&dirs, getenv("WESTLAKE_NATIVE_LIB_DIR"));
  PFCutAppendPathList(&dirs, getenv("WESTLAKE_LIBRARY_PATH"));
  PFCutAppendPathList(&dirs, getenv("LD_LIBRARY_PATH"));
  dirs.emplace_back("/data/local/tmp/westlake/app_lib");
  dirs.emplace_back("/data/local/tmp/westlake");

  for (const std::string& dir : dirs) {
    if (dir.empty()) {
      continue;
    }
    std::string path = dir;
    if (path.back() != '/') {
      path.push_back('/');
    }
    path += file_name;
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), R_OK) == 0) {
      ObjPtr<mirror::String> out = mirror::String::AllocFromModifiedUtf8(self, path.c_str());
      result->SetL(out);
      static thread_local int find_library_hit_count = 0;
      if (find_library_hit_count < 80) {
        find_library_hit_count++;
        fprintf(stderr, "[PFCUT] DexPathList.findLibrary %s -> %s\n",
                library_name.c_str(), path.c_str());
        fflush(stderr);
      }
      return true;
    }
  }

  static thread_local int find_library_miss_count = 0;
  if (find_library_miss_count < 80) {
    find_library_miss_count++;
    fprintf(stderr, "[PFCUT] DexPathList.findLibrary %s -> <missing>\n",
            library_name.c_str());
    fflush(stderr);
  }
  result->SetL(nullptr);
  return true;
}

static inline bool PFCutTryFileSystemsGetDefaultFallback(ArtMethod* called_method,
                                                         Thread* self,
                                                         JValue* result,
                                                         uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 0u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/nio/file/FileSystems;") ||
      strcmp(WlSafeName(called_method), "getDefault") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "L") != 0) {
    return false;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<12> hs(self);
  Handle<mirror::ClassLoader> h_loader(
      hs.NewHandle(called_method->GetDeclaringClass()->GetClassLoader()));
  Handle<mirror::Class> h_provider_class(
      hs.NewHandle(class_linker->FindClass(self, "Lsun/nio/fs/LinuxFileSystemProvider;", h_loader)));
  Handle<mirror::Class> h_unix_provider_class(
      hs.NewHandle(class_linker->FindClass(self, "Lsun/nio/fs/UnixFileSystemProvider;", h_loader)));
  Handle<mirror::Class> h_fs_class(
      hs.NewHandle(class_linker->FindClass(self, "Lsun/nio/fs/LinuxFileSystem;", h_loader)));
  Handle<mirror::Class> h_path_class(
      hs.NewHandle(class_linker->FindClass(self, "Lsun/nio/fs/UnixPath;", h_loader)));
  if (self->IsExceptionPending()) {
    self->ClearException();
  }
  if (h_provider_class == nullptr || h_unix_provider_class == nullptr ||
      h_fs_class == nullptr || h_path_class == nullptr) {
    result->SetL(nullptr);
    return true;
  }
  if (!class_linker->EnsureInitialized(self, h_provider_class, true, true) ||
      !class_linker->EnsureInitialized(self, h_unix_provider_class, true, true) ||
      !class_linker->EnsureInitialized(self, h_fs_class, true, true) ||
      !class_linker->EnsureInitialized(self, h_path_class, true, true)) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return true;
  }

  static constexpr const char* kDefaultDir = "/data/local/tmp/westlake";
  Handle<mirror::Object> h_provider(hs.NewHandle(h_provider_class->AllocObject(self)));
  Handle<mirror::Object> h_fs(hs.NewHandle(h_fs_class->AllocObject(self)));
  Handle<mirror::ByteArray> h_default_dir(
      hs.NewHandle(mirror::ByteArray::Alloc(self, static_cast<int32_t>(strlen(kDefaultDir)))));
  Handle<mirror::ByteArray> h_root_path(hs.NewHandle(mirror::ByteArray::Alloc(self, 1)));
  Handle<mirror::Object> h_root(hs.NewHandle(h_path_class->AllocObject(self)));
  if (h_provider == nullptr || h_fs == nullptr ||
      h_default_dir == nullptr || h_root_path == nullptr || h_root == nullptr) {
    result->SetL(nullptr);
    return true;
  }
  memcpy(h_default_dir->GetData(), kDefaultDir, strlen(kDefaultDir));
  h_root_path->GetData()[0] = '/';

  ArtField* path_fs_field = h_path_class->FindInstanceField("fs", "Lsun/nio/fs/UnixFileSystem;");
  ArtField* path_path_field = h_path_class->FindInstanceField("path", "[B");
  ArtField* fs_provider_field =
      h_fs_class->FindInstanceField("provider", "Lsun/nio/fs/UnixFileSystemProvider;");
  ArtField* fs_default_dir_field = h_fs_class->FindInstanceField("defaultDirectory", "[B");
  ArtField* fs_resolve_field =
      h_fs_class->FindInstanceField("needToResolveAgainstDefaultDirectory", "Z");
  ArtField* fs_root_field =
      h_fs_class->FindInstanceField("rootDirectory", "Lsun/nio/fs/UnixPath;");
  if (path_fs_field == nullptr || path_path_field == nullptr ||
      fs_provider_field == nullptr || fs_default_dir_field == nullptr ||
      fs_resolve_field == nullptr || fs_root_field == nullptr) {
    result->SetL(nullptr);
    return true;
  }
  path_fs_field->SetObject<false>(h_root.Get(), h_fs.Get());
  path_path_field->SetObject<false>(h_root.Get(), h_root_path.Get());
  fs_provider_field->SetObject<false>(h_fs.Get(), h_provider.Get());
  fs_default_dir_field->SetObject<false>(h_fs.Get(), h_default_dir.Get());
  fs_resolve_field->SetBoolean<false>(h_fs.Get(), false);
  fs_root_field->SetObject<false>(h_fs.Get(), h_root.Get());

  ArtField* provider_fs_field =
      h_provider_class->FindInstanceField("theFileSystem", "Lsun/nio/fs/UnixFileSystem;");
  if (provider_fs_field != nullptr) {
    provider_fs_field->SetObject<false>(h_provider.Get(), h_fs.Get());
  }

  Handle<mirror::Class> h_holder_class(
      hs.NewHandle(class_linker->FindClass(
          self, "Ljava/nio/file/FileSystems$DefaultFileSystemHolder;", h_loader)));
  if (self->IsExceptionPending()) {
    self->ClearException();
  }
  if (h_holder_class != nullptr) {
    ArtField* default_fs_field =
        h_holder_class->FindDeclaredStaticField("defaultFileSystem", "Ljava/nio/file/FileSystem;");
    if (default_fs_field != nullptr) {
      default_fs_field->SetObject<false>(h_holder_class.Get(), h_fs.Get());
    }
  }

  static thread_local int filesystem_fallback_count = 0;
  if (filesystem_fallback_count < 40) {
    filesystem_fallback_count++;
    fprintf(stderr,
            "[PFCUT] FileSystems.getDefault fallback -> LinuxFileSystem(/data/local/tmp/westlake)\n");
    fflush(stderr);
  }
  result->SetL(h_fs.Get());
  return true;
}

template <bool is_range>
static inline bool PFCutTryMcdLoggerNoop(ArtMethod* called_method,
                                         ShadowFrame& shadow_frame,
                                         JValue* result,
                                         uint16_t number_of_inputs,
                                         uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                         uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  if (!called_method->GetDeclaringClass()->DescriptorEquals(
          "Lcom/mcdonalds/androidsdk/core/logger/McDLog;")) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || shorty_len == 0u) {
    return false;
  }

  static thread_local int mcd_logger_noop_count = 0;
  if (mcd_logger_noop_count < 80) {
    mcd_logger_noop_count++;
    fprintf(stderr,
            "[PFCUT] McDLog noop %s shorty=%s quick=%p jni=%p native=%d code=%p\n",
            called_method->PrettyMethod().c_str(),
            shorty,
            called_method->GetEntryPointFromQuickCompiledCode(),
            called_method->IsNative() ? called_method->GetEntryPointFromJni() : nullptr,
            called_method->IsNative() ? 1 : 0,
            called_method->GetCodeItem());
    const auto reg_at = [&](uint32_t input_index) -> uint32_t {
      return is_range ? (vregC + input_index) : arg[input_index];
    };
    if (number_of_inputs > 0u) {
      const uint32_t first_arg = called_method->IsStatic() ? 0u : 1u;
      for (uint32_t i = first_arg; i < number_of_inputs; ++i) {
        fprintf(stderr, "[PFCUT] McDLog arg%u ", i - first_arg);
        PFCutPrintObjectSummary(shadow_frame.GetVRegReference(reg_at(i)), 0);
        fprintf(stderr, "\n");
      }
    }
    fflush(stderr);
  }

  // McDLog is app telemetry/logging only. Keeping it inert avoids executing
  // New Relic-instrumented logger bodies while preserving app-visible defaults.
  result->SetJ(0);
  return true;
}

static inline bool PFCutTryMcdJustFlipEventNoop(ArtMethod* called_method,
                                                JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->GetDeclaringClass() == nullptr ||
      strcmp(WlSafeName(called_method), "c") != 0 ||
      called_method->IsStatic() ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Lcom/mcdonalds/justflip_kmm/flag_providers/JustFlipBase;")) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "VL") != 0) {
    return false;
  }

  static thread_local int justflip_event_noop_count = 0;
  if (justflip_event_noop_count < 40 || WestlakeTraceVerboseCalls()) {
    justflip_event_noop_count++;
    fprintf(stderr,
            "[PFCUT-MCD] JustFlipBase.c event emission noop %s shorty=%s\n",
            called_method->PrettyMethod().c_str(),
            shorty);
    fflush(stderr);
  }
  result->SetJ(0);
  return true;
}

static inline bool PFCutSetMcdNetworkStringResponse(ArtMethod* called_method,
                                                    JValue* result,
                                                    int32_t status,
                                                    const std::string& body_text)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  if (self == nullptr || called_method == nullptr || result == nullptr ||
      called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<4> hs(self);
  Handle<mirror::ClassLoader> h_loader(
      hs.NewHandle(called_method->GetDeclaringClass()->GetClassLoader()));
  Handle<mirror::Class> h_response_class(hs.NewHandle(class_linker->FindClass(
      self, "Lcom/mcdonalds/mcdcoreapp/network/Response;", h_loader)));
  if (h_response_class == nullptr || self->IsExceptionPending()) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return true;
  }
  if (!class_linker->EnsureInitialized(self, h_response_class, true, true)) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return true;
  }

  Handle<mirror::Object> h_response(hs.NewHandle(h_response_class->AllocObject(self)));
  if (h_response == nullptr || self->IsExceptionPending()) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return true;
  }

  ArtField* status_field = h_response_class->FindInstanceField("b", "I");
  ArtField* body_field = h_response_class->FindInstanceField("d", "Ljava/lang/Object;");
  if (status_field != nullptr) {
    status_field->SetInt<false>(h_response.Get(), status);
  }
  if (body_field != nullptr) {
    ObjPtr<mirror::String> body =
        mirror::String::AllocFromModifiedUtf8(self, body_text.c_str());
    if (body != nullptr && !self->IsExceptionPending()) {
      Handle<mirror::String> h_body(hs.NewHandle(body));
      body_field->SetObject<false>(h_response.Get(), h_body.Get());
    } else if (self->IsExceptionPending()) {
      self->ClearException();
    }
  }
  result->SetL(h_response.Get());
  return true;
}

static inline bool PFCutLogMcdRequestProvider(Thread* self,
                                              ArtMethod* called_method,
                                              ObjPtr<mirror::Object> provider,
                                              std::string* out_url,
                                              int32_t* out_method_type,
                                              std::string* out_response_type)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (out_url != nullptr) {
    *out_url = "<unknown>";
  }
  if (out_method_type != nullptr) {
    *out_method_type = -1;
  }
  if (out_response_type != nullptr) {
    *out_response_type = "<unknown>";
  }
  if (self == nullptr || called_method == nullptr || provider == nullptr ||
      provider->GetClass() == nullptr) {
    return false;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<4> hs(self);
  Handle<mirror::Object> h_provider(hs.NewHandle(provider));
  Handle<mirror::ClassLoader> h_loader(
      hs.NewHandle(called_method->GetDeclaringClass()->GetClassLoader()));
  Handle<mirror::Class> h_provider_interface(hs.NewHandle(class_linker->FindClass(
      self, "Lcom/mcdonalds/mcdcoreapp/network/RequestProvider;", h_loader)));
  if (h_provider_interface == nullptr || self->IsExceptionPending()) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    return false;
  }

  ArtMethod* url_method = h_provider_interface->FindInterfaceMethod(
      "b", "()Ljava/lang/String;", kRuntimePointerSize);
  ArtMethod* method_type_method = h_provider_interface->FindInterfaceMethod(
      "a", "()I", kRuntimePointerSize);
  ArtMethod* response_type_method = h_provider_interface->FindInterfaceMethod(
      "c", "()Ljava/lang/Class;", kRuntimePointerSize);

  std::string url = "<unknown>";
  int32_t method_type = -1;
  std::string response_type = "<unknown>";
  ScopedObjectAccessUnchecked soa(self);
  ScopedJniEnvLocalRefState env_state(soa.Env());
  jobject receiver = soa.AddLocalReference<jobject>(h_provider.Get());

  if (url_method != nullptr) {
    JValue url_result =
        InvokeVirtualOrInterfaceWithJValues(soa, receiver, url_method, nullptr);
    if (self->IsExceptionPending()) {
      self->ClearException();
      url = "<exception>";
    } else if (url_result.GetL() != nullptr &&
               url_result.GetL()->GetClass()->DescriptorEquals("Ljava/lang/String;")) {
      url = url_result.GetL()->AsString()->ToModifiedUtf8();
    }
  }

  if (method_type_method != nullptr) {
    JValue method_result =
        InvokeVirtualOrInterfaceWithJValues(soa, receiver, method_type_method, nullptr);
    if (self->IsExceptionPending()) {
      self->ClearException();
      method_type = -2;
    } else {
      method_type = method_result.GetI();
    }
  }

  if (response_type_method != nullptr) {
    JValue response_result =
        InvokeVirtualOrInterfaceWithJValues(soa, receiver, response_type_method, nullptr);
    if (self->IsExceptionPending()) {
      self->ClearException();
      response_type = "<exception>";
    } else if (response_result.GetL() != nullptr && response_result.GetL()->IsClass()) {
      response_type = response_result.GetL()->AsClass()->PrettyDescriptor();
    }
  }

  static thread_local int mcd_network_provider_count = 0;
  if (mcd_network_provider_count < 120 || WestlakeTraceMcdCalls()) {
    mcd_network_provider_count++;
    fprintf(stderr,
            "[PFCUT-MCD-NET] provider method=%s httpMethodType=%d response=%s url=%s\n",
            called_method->PrettyMethod().c_str(),
            method_type,
            response_type.c_str(),
            url.c_str());
    fflush(stderr);
  }
  if (out_url != nullptr) {
    *out_url = url;
  }
  if (out_method_type != nullptr) {
    *out_method_type = method_type;
  }
  if (out_response_type != nullptr) {
    *out_response_type = response_type;
  }
  return true;
}

static inline bool PFCutTryMcdBridgeHttpGetResponse(ArtMethod* called_method,
                                                    const std::string& url,
                                                    JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  if (self == nullptr || called_method == nullptr || result == nullptr ||
      url.empty() || url == "<unknown>" || url == "<exception>") {
    return false;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<6> hs(self);
  Handle<mirror::ClassLoader> h_loader(
      hs.NewHandle(called_method->GetDeclaringClass()->GetClassLoader()));
  Handle<mirror::Class> h_launcher(hs.NewHandle(class_linker->FindClass(
      self, "Lcom/westlake/engine/WestlakeLauncher;", h_loader)));
  if (h_launcher == nullptr || self->IsExceptionPending()) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    return false;
  }
  if (!class_linker->EnsureInitialized(self, h_launcher, true, true)) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    return false;
  }

  ArtMethod* bridge_method = h_launcher->FindClassMethod(
      "bridgeHttpGetBytes", "(Ljava/lang/String;II)[B", kRuntimePointerSize);
  ArtMethod* last_status_method = h_launcher->FindClassMethod(
      "bridgeHttpLastStatus", "()I", kRuntimePointerSize);
  ArtMethod* last_error_method = h_launcher->FindClassMethod(
      "bridgeHttpLastError", "()Ljava/lang/String;", kRuntimePointerSize);
  if (bridge_method == nullptr || last_status_method == nullptr) {
    return false;
  }

  Handle<mirror::String> h_url(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, url.c_str())));
  if (h_url == nullptr || self->IsExceptionPending()) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    return false;
  }

  ScopedObjectAccessUnchecked soa(self);
  ScopedJniEnvLocalRefState env_state(soa.Env());
  jvalue args[3] = {};
  args[0].l = soa.AddLocalReference<jobject>(h_url.Get());
  args[1].i = 512 * 1024;
  args[2].i = 20000;
  JValue bytes_result = InvokeWithJValues(soa, nullptr, bridge_method, args);
  bool bridge_threw = false;
  if (self->IsExceptionPending()) {
    self->ClearException();
    bridge_threw = true;
  }

  int32_t status = -599;
  JValue status_result = InvokeWithJValues(soa, nullptr, last_status_method, nullptr);
  if (self->IsExceptionPending()) {
    self->ClearException();
  } else {
    status = status_result.GetI();
  }

  std::string error = "";
  if (last_error_method != nullptr) {
    JValue error_result = InvokeWithJValues(soa, nullptr, last_error_method, nullptr);
    if (self->IsExceptionPending()) {
      self->ClearException();
    } else if (error_result.GetL() != nullptr &&
               error_result.GetL()->GetClass()->DescriptorEquals("Ljava/lang/String;")) {
      error = error_result.GetL()->AsString()->ToModifiedUtf8();
    }
  }

  std::string body = "{}";
  ObjPtr<mirror::Object> bytes_object = bytes_result.GetL();
  if (!bridge_threw && bytes_object != nullptr && bytes_object->IsByteArray()) {
    ObjPtr<mirror::ByteArray> bytes = bytes_object->AsByteArray();
    const int32_t length = bytes->GetLength();
    if (length > 0) {
      body.assign(reinterpret_cast<const char*>(bytes->GetData()),
                  reinterpret_cast<const char*>(bytes->GetData()) + length);
    }
  }
  if (status <= 0) {
    status = 503;
  }

  static thread_local int mcd_network_bridge_count = 0;
  if (mcd_network_bridge_count < 120 || WestlakeTraceMcdCalls()) {
    mcd_network_bridge_count++;
    fprintf(stderr,
            "[PFCUT-MCD-NET] bridge response status=%d bytes=%zu error=%s url=%s\n",
            status,
            body.size(),
            error.empty() ? "<none>" : error.c_str(),
            url.c_str());
    fflush(stderr);
  }

  return PFCutSetMcdNetworkStringResponse(called_method, result, status, body);
}

template <bool is_range>
static inline bool PFCutTryMcdNetworkBoundaryNoop(ArtMethod* called_method,
                                                  Thread* self,
                                                  ShadowFrame& shadow_frame,
                                                  JValue* result,
                                                  uint16_t number_of_inputs,
                                                  uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                  uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  std::string descriptor_storage;
  const char* descriptor =
      called_method->GetDeclaringClass()->GetDescriptor(&descriptor_storage);
  if (descriptor == nullptr ||
      strcmp(descriptor, "Lcom/mcdonalds/mcdcoreapp/network/McDRequestManager;") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  const char* name = WlSafeName(called_method);
  if (shorty == nullptr || name == nullptr) {
    return false;
  }

  const bool async_request_entry =
      !called_method->IsStatic() && strcmp(name, "e") == 0 && strcmp(shorty, "VLL") == 0;
  const bool request_callable =
      called_method->IsStatic() &&
      (strcmp(name, "a") == 0 || strcmp(name, "d") == 0) &&
      strcmp(shorty, "LL") == 0;
  if (!async_request_entry && !request_callable) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };
  std::string provider_url;
  int32_t provider_method_type = -1;
  std::string provider_response_type;
  if (number_of_inputs >= (called_method->IsStatic() ? 1u : 2u)) {
    const uint32_t provider_slot = called_method->IsStatic() ? 0u : 1u;
    PFCutLogMcdRequestProvider(
        self,
        called_method,
        shadow_frame.GetVRegReference(reg_at(provider_slot)),
        &provider_url,
        &provider_method_type,
        &provider_response_type);
  }

  if (async_request_entry) {
    return false;
  }

  static thread_local int mcd_network_noop_count = 0;
  if (mcd_network_noop_count < 80 || WestlakeTraceMcdCalls()) {
    mcd_network_noop_count++;
    fprintf(stderr,
            "[PFCUT-MCD] network boundary live passthrough %s shorty=%s quick=%p code=%p\n",
            called_method->PrettyMethod().c_str(),
            shorty,
            called_method->GetEntryPointFromQuickCompiledCode(),
            called_method->GetCodeItem());
    fflush(stderr);
  }

  if (provider_method_type == 0 &&
      PFCutTryMcdBridgeHttpGetResponse(called_method, provider_url, result)) {
    return true;
  }
  return PFCutSetMcdNetworkStringResponse(called_method, result, 503, "{}");
}

template <bool is_range>
static inline bool PFCutTryMcdPerfAnalyticsNoop(ArtMethod* called_method,
                                                Thread* self,
                                                ShadowFrame& shadow_frame,
                                                JValue* result,
                                                uint16_t number_of_inputs,
                                                uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  std::string descriptor_storage;
  const char* descriptor = called_method->GetDeclaringClass()->GetDescriptor(&descriptor_storage);
  if (descriptor == nullptr ||
      (strstr(descriptor, "Lcom/mcdonalds/mcdcoreapp/performanalytics/") == nullptr &&
       strstr(descriptor, "Lcom/mcdonalds/app/performanalytics/") == nullptr)) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || shorty_len == 0u) {
    return false;
  }

  static thread_local int mcd_perf_noop_count = 0;
  if (mcd_perf_noop_count < 120) {
    mcd_perf_noop_count++;
    fprintf(stderr,
            "[PFCUT] McD perf noop %s shorty=%s quick=%p jni=%p native=%d code=%p\n",
            called_method->PrettyMethod().c_str(),
            shorty,
            called_method->GetEntryPointFromQuickCompiledCode(),
            called_method->IsNative() ? called_method->GetEntryPointFromJni() : nullptr,
            called_method->IsNative() ? 1 : 0,
            called_method->GetCodeItem());
    const auto reg_at = [&](uint32_t input_index) -> uint32_t {
      return is_range ? (vregC + input_index) : arg[input_index];
    };
    if (number_of_inputs > 0u) {
      const uint32_t first_arg = called_method->IsStatic() ? 0u : 1u;
      for (uint32_t i = first_arg; i < number_of_inputs; ++i) {
        fprintf(stderr, "[PFCUT] McD perf arg%u ", i - first_arg);
        PFCutPrintObjectSummary(shadow_frame.GetVRegReference(reg_at(i)), 0);
        fprintf(stderr, "\n");
      }
    }
    fflush(stderr);
  }

  const char* name = WlSafeName(called_method);
  if (name != nullptr &&
      strcmp(name, "H") == 0 &&
      called_method->IsStatic() &&
      called_method->GetDeclaringClass()->DescriptorEquals(
          "Lcom/mcdonalds/mcdcoreapp/performanalytics/PerfAnalyticsInteractor;") &&
      strcmp(shorty, "L") == 0) {
    result->SetL(called_method->GetDeclaringClass()->AllocObject(self));
    return true;
  }
  if (name != nullptr &&
      strcmp(name, "f") == 0 &&
      called_method->IsStatic() &&
      called_method->GetDeclaringClass()->DescriptorEquals(
          "Lcom/mcdonalds/mcdcoreapp/performanalytics/PerformanceAnalyticsHelper;") &&
      strcmp(shorty, "L") == 0) {
    result->SetL(called_method->GetDeclaringClass()->AllocObject(self));
    return true;
  }

  result->SetJ(0);
  return true;
}

static inline bool PFCutTryNumberFormatCurrencyFallback(ArtMethod* called_method,
                                                        Thread* self,
                                                        JValue* result,
                                                        uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 1u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/text/NumberFormat;") ||
      strcmp(WlSafeName(called_method), "getCurrencyInstance") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  static thread_local int currency_fallback_count = 0;
  if (currency_fallback_count < 40) {
    currency_fallback_count++;
    fprintf(stderr,
            "[PFCUT] NumberFormat.getCurrencyInstance(Locale) fallback -> null (stable dashboard mode)\n");
    fflush(stderr);
  }
  result->SetL(nullptr);
  return true;
}

static inline bool PFCutTryVMClassLoaderGetResourceFallback(ArtMethod* called_method,
                                                            JValue* result,
                                                            uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 1u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/VMClassLoader;") ||
      strcmp(WlSafeName(called_method), "getResource") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  static thread_local int get_resource_fallback_count = 0;
  if (get_resource_fallback_count < 40) {
    get_resource_fallback_count++;
    fprintf(stderr,
            "[PFCUT] VMClassLoader.getResource(String) fallback -> null\n");
    fflush(stderr);
  }
  result->SetL(nullptr);
  return true;
}

static inline bool PFCutTryVMClassLoaderGetResourcesFallback(ArtMethod* called_method,
                                                             Thread* self,
                                                             JValue* result,
                                                             uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 1u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/VMClassLoader;") ||
      strcmp(WlSafeName(called_method), "getResources") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  StackHandleScope<2> hs(self);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::ClassLoader> h_loader(
      hs.NewHandle(called_method->GetDeclaringClass()->GetClassLoader()));
  Handle<mirror::Class> h_collections(
      hs.NewHandle(class_linker->FindClass(self, "Ljava/util/Collections;", h_loader)));
  if (h_collections == nullptr) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return true;
  }
  if (!class_linker->EnsureInitialized(self, h_collections, true, true)) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return true;
  }
  ArtMethod* empty_list_method =
      h_collections->FindClassMethod("emptyList", "()Ljava/util/List;", kRuntimePointerSize);
  if (empty_list_method == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  ScopedObjectAccessUnchecked soa(self);
  ScopedJniEnvLocalRefState env_state(soa.Env());
  JValue empty_list = InvokeWithJValues(soa, nullptr, empty_list_method, nullptr);
  if (self->IsExceptionPending()) {
    self->ClearException();
    result->SetL(nullptr);
    return true;
  }

  static thread_local int get_resources_fallback_count = 0;
  if (get_resources_fallback_count < 40) {
    get_resources_fallback_count++;
    fprintf(stderr,
            "[PFCUT] VMClassLoader.getResources(String) fallback -> Collections.emptyList\n");
    fflush(stderr);
  }
  result->SetL(empty_list.GetL());
  return true;
}

template <bool is_range>
static inline bool PFCutTryIcuDataPathPropertyFallback(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      (number_of_inputs != 1u && number_of_inputs != 2u)) {
    return false;
  }

  const bool is_icu_config =
      called_method->GetDeclaringClass()->DescriptorEquals("Landroid/icu/impl/ICUConfig;") &&
      strcmp(WlSafeName(called_method), "get") == 0;
  const bool is_system_property =
      called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/System;") &&
      strcmp(WlSafeName(called_method), "getProperty") == 0;
  if (!is_icu_config && !is_system_property) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr ||
      !((number_of_inputs == 1u && strcmp(shorty, "LL") == 0) ||
        (number_of_inputs == 2u && strcmp(shorty, "LLL") == 0))) {
    return false;
  }

  const uint32_t key_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> key_obj = shadow_frame.GetVRegReference(key_reg);
  if (key_obj == nullptr || !key_obj->IsString()) {
    return false;
  }

  const std::string key = key_obj->AsString()->ToModifiedUtf8();
  if (key != "android.icu.impl.ICUBinary.dataPath") {
    return false;
  }

  static constexpr const char* kIcuDataPath =
      "/data/local/tmp/westlake/icu/icudt75l.dat:"
      "/data/local/tmp/westlake/icu:"
      "/apex/com.android.i18n/etc/icu/icudt75l.dat:"
      "/apex/com.android.i18n/etc/icu";
  ObjPtr<mirror::String> path = mirror::String::AllocFromModifiedUtf8(self, kIcuDataPath);
  result->SetL(path);

  static thread_local int icu_data_path_count = 0;
  if (icu_data_path_count < 40) {
    icu_data_path_count++;
    fprintf(stderr,
            "[PFCUT-ICU] %s dataPath -> %s\n",
            called_method->PrettyMethod().c_str(),
            kIcuDataPath);
    fflush(stderr);
  }
  return true;
}

static inline bool PFCutTryCurrencyInstanceFallback(ArtMethod* called_method,
                                                    Thread* self,
                                                    JValue* result,
                                                    uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 1u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/util/Currency;") ||
      strcmp(WlSafeName(called_method), "getInstance") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  std::string pretty = called_method->PrettyMethod();
  if (pretty.find("java.util.Locale") == std::string::npos) {
    return false;
  }

  StackHandleScope<3> hs(self);
  Handle<mirror::Class> h_currency_class(hs.NewHandle(called_method->GetDeclaringClass()));
  Handle<mirror::Object> h_currency(hs.NewHandle(h_currency_class->AllocObject(self)));
  if (h_currency == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  Handle<mirror::String> h_code(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "USD")));
  ArtField* code_field =
      h_currency_class->FindDeclaredInstanceField("currencyCode", "Ljava/lang/String;");
  if (code_field != nullptr && h_code != nullptr) {
    code_field->SetObject<false>(h_currency.Get(), h_code.Get());
  }
  if (self->IsExceptionPending()) {
    self->ClearException();
  }

  static thread_local int currency_instance_fallback_count = 0;
  if (currency_instance_fallback_count < 40) {
    currency_instance_fallback_count++;
    fprintf(stderr,
            "[PFCUT] Currency.getInstance(Locale) fallback -> USD object\n");
    fflush(stderr);
  }
  result->SetL(h_currency.Get());
  return true;
}

template <bool is_range>
static inline bool PFCutTryCurrencyMethodFallback(ArtMethod* called_method,
                                                  Thread* self,
                                                  ShadowFrame& shadow_frame,
                                                  JValue* result,
                                                  uint16_t number_of_inputs,
                                                  uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                  uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Ljava/util/Currency;")) {
    return false;
  }

  const char* name = WlSafeName(called_method);
  if (strcmp(name, "getSymbol") != 0 &&
      strcmp(name, "getDisplayName") != 0 &&
      strcmp(name, "getDefaultFractionDigits") != 0 &&
      strcmp(name, "getNumericCode") != 0 &&
      strcmp(name, "toString") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr) {
    return false;
  }

  const bool returns_string = strcmp(name, "getSymbol") == 0 ||
                              strcmp(name, "getDisplayName") == 0 ||
                              strcmp(name, "toString") == 0;
  if (returns_string) {
    const bool has_locale_arg =
        (strcmp(name, "getSymbol") == 0 || strcmp(name, "getDisplayName") == 0) &&
        strcmp(shorty, "LL") == 0 &&
        number_of_inputs == 2u;
    const bool no_arg_string = strcmp(shorty, "L") == 0 && number_of_inputs == 1u;
    if (!has_locale_arg && !no_arg_string) {
      return false;
    }
  } else if (strcmp(shorty, "I") != 0 || number_of_inputs != 1u) {
    return false;
  }

  const uint32_t receiver_reg = is_range ? vregC : arg[0];
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    if (returns_string) {
      result->SetL(nullptr);
    } else {
      result->SetI(0);
    }
    return true;
  }

  std::string code = "USD";
  ArtField* code_field =
      receiver->GetClass()->FindDeclaredInstanceField("currencyCode", "Ljava/lang/String;");
  if (code_field != nullptr) {
    ObjPtr<mirror::Object> code_obj = code_field->GetObject(receiver);
    if (code_obj != nullptr && code_obj->IsString()) {
      std::string actual = code_obj->AsString()->ToModifiedUtf8();
      if (!actual.empty()) {
        code = actual;
      }
    }
  }

  if (strcmp(name, "getDefaultFractionDigits") == 0) {
    result->SetI(code == "JPY" ? 0 : 2);
  } else if (strcmp(name, "getNumericCode") == 0) {
    result->SetI(code == "USD" ? 840 : 0);
  } else {
    const char* value = code.c_str();
    std::string display;
    if (strcmp(name, "getSymbol") == 0 && code == "USD") {
      value = "$";
    } else if (strcmp(name, "getDisplayName") == 0 && code == "USD") {
      value = "US Dollar";
    } else if (strcmp(name, "getDisplayName") == 0) {
      display = code;
      value = display.c_str();
    }
    ObjPtr<mirror::String> out = mirror::String::AllocFromModifiedUtf8(self, value);
    result->SetL(out);
  }

  static thread_local int currency_method_fallback_count = 0;
  if (currency_method_fallback_count < 80) {
    currency_method_fallback_count++;
    fprintf(stderr,
            "[PFCUT] Currency.%s fallback code=%s shorty=%s\n",
            name,
            code.c_str(),
            shorty);
    fflush(stderr);
  }
  return true;
}

static inline bool PFCutTryULocaleForLocaleFallback(ArtMethod* called_method,
                                                    Thread* self,
                                                    JValue* result,
                                                    uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 1u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Landroid/icu/util/ULocale;") ||
      strcmp(WlSafeName(called_method), "forLocale") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  StackHandleScope<3> hs(self);
  Handle<mirror::Class> h_ulocale_class(hs.NewHandle(called_method->GetDeclaringClass()));
  Handle<mirror::Object> h_ulocale(hs.NewHandle(h_ulocale_class->AllocObject(self)));
  if (h_ulocale == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  Handle<mirror::String> h_locale_id(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "en_US")));
  ArtField* locale_id_field =
      h_ulocale_class->FindDeclaredInstanceField("localeID", "Ljava/lang/String;");
  if (locale_id_field != nullptr && h_locale_id != nullptr) {
    locale_id_field->SetObject<false>(h_ulocale.Get(), h_locale_id.Get());
  }
  if (self->IsExceptionPending()) {
    self->ClearException();
  }

  static thread_local int ulocale_fallback_count = 0;
  if (ulocale_fallback_count < 40) {
    ulocale_fallback_count++;
    fprintf(stderr,
            "[PFCUT] ULocale.forLocale(Locale) fallback -> en_US object\n");
    fflush(stderr);
  }
  result->SetL(h_ulocale.Get());
  return true;
}

static inline bool PFCutTryULocaleGetDefaultFallback(ArtMethod* called_method,
                                                     Thread* self,
                                                     JValue* result,
                                                     uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Landroid/icu/util/ULocale;") ||
      strcmp(WlSafeName(called_method), "getDefault") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr ||
      !((number_of_inputs == 0u && strcmp(shorty, "L") == 0) ||
        (number_of_inputs == 1u && strcmp(shorty, "LL") == 0))) {
    return false;
  }

  StackHandleScope<3> hs(self);
  Handle<mirror::Class> h_ulocale_class(hs.NewHandle(called_method->GetDeclaringClass()));
  Handle<mirror::Object> h_ulocale(hs.NewHandle(h_ulocale_class->AllocObject(self)));
  if (h_ulocale == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  Handle<mirror::String> h_locale_id(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "en_US")));
  ArtField* locale_id_field =
      h_ulocale_class->FindDeclaredInstanceField("localeID", "Ljava/lang/String;");
  if (locale_id_field != nullptr && h_locale_id != nullptr) {
    locale_id_field->SetObject<false>(h_ulocale.Get(), h_locale_id.Get());
  }
  if (self->IsExceptionPending()) {
    self->ClearException();
  }

  static thread_local int ulocale_default_fallback_count = 0;
  if (ulocale_default_fallback_count < 40) {
    ulocale_default_fallback_count++;
    fprintf(stderr,
            "[PFCUT] ULocale.getDefault fallback -> en_US object\n");
    fflush(stderr);
  }
  result->SetL(h_ulocale.Get());
  return true;
}

template <bool is_range>
// WESTLAKE §667 (2026-08-16): NEVER fabricate a null KClass — fall through to the real method.
// This shim builds a minimal kotlin ClassReference as a *fallback*, but on ANY failure it did
// `result->SetL(nullptr); return true;`, i.e. it CLAIMED the call and handed back a null KClass with
// no exception. That is the origin of Toutiao's blocker: androidx lifecycle 2.9's
// `ViewModelProvider.get(Class)` is a thin adapter that calls
// `JvmClassMappingKt.getKotlinClass(Class)` and passes the result to `get(KClass)`, whose Kotlin
// intrinsic then throws "Parameter specified as non-null is null: … parameter modelClass" one frame
// later. CONST_CLASS and the whole argument path were verified innocent (§664/§666).
// `kotlin.reflect.jvm.internal.ReflectionFactoryImpl` IS bundled (classes14.dex) and its
// implementation is trivial, so returning FALSE lets the app's own, correct code run.
// Logging is file-gated (`touch /data/local/tmp/asx/ARGNULL`) and names which exit fired.
static inline bool PFCutTryKotlinReflectionFallback(ArtMethod* called_method,
                                                    Thread* self,
                                                    ShadowFrame& shadow_frame,
                                                    JValue* result,
                                                    uint16_t number_of_inputs,
                                                    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->GetDeclaringClass() == nullptr ||
      called_method->IsStatic()) {
    return false;
  }

  if (!called_method->GetDeclaringClass()->DescriptorEquals(
          "Lkotlin/reflect/jvm/internal/ReflectionFactoryImpl;")) {
    return false;
  }

  const char* name = WlSafeName(called_method);
  if (name == nullptr || strcmp(name, "getOrCreateKotlinClass") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr ||
      !((strcmp(shorty, "LL") == 0 && number_of_inputs == 2u) ||
        (strcmp(shorty, "LLL") == 0 && number_of_inputs == 3u))) {
    return false;
  }

  const uint32_t class_arg_reg = is_range ? vregC + 1u : arg[1];
  ObjPtr<mirror::Object> java_class = shadow_frame.GetVRegReference(class_arg_reg);
  if (java_class == nullptr) {
    if (access("/data/local/tmp/asx/ARGNULL", F_OK) == 0) {
      static thread_local int wl_kr = 0;
      if (wl_kr < 8) { wl_kr++;
        fprintf(stderr, "[WL-KREFL] getOrCreateKotlinClass fallback exit reason=1"
                        " -> deferring to the real method\n");
        fflush(stderr); }
    }
    return false;  // §667: do NOT claim the call; let real Kotlin reflection run.
  }

  StackHandleScope<3> hs(self);
  Handle<mirror::Object> h_java_class(hs.NewHandle(java_class));
  Handle<mirror::ClassLoader> h_loader(
      hs.NewHandle(called_method->GetDeclaringClass()->GetClassLoader()));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> h_class_reference(hs.NewHandle(
      class_linker->FindClass(self, "Lkotlin/jvm/internal/ClassReference;", h_loader)));
  if (h_class_reference == nullptr) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    if (access("/data/local/tmp/asx/ARGNULL", F_OK) == 0) {
      static thread_local int wl_kr = 0;
      if (wl_kr < 8) { wl_kr++;
        fprintf(stderr, "[WL-KREFL] getOrCreateKotlinClass fallback exit reason=2"
                        " -> deferring to the real method\n");
        fflush(stderr); }
    }
    return false;  // §667: do NOT claim the call; let real Kotlin reflection run.
  }

  if (!class_linker->EnsureInitialized(self, h_class_reference, true, true)) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    if (access("/data/local/tmp/asx/ARGNULL", F_OK) == 0) {
      static thread_local int wl_kr = 0;
      if (wl_kr < 8) { wl_kr++;
        fprintf(stderr, "[WL-KREFL] getOrCreateKotlinClass fallback exit reason=3"
                        " -> deferring to the real method\n");
        fflush(stderr); }
    }
    return false;  // §667: do NOT claim the call; let real Kotlin reflection run.
  }

  ObjPtr<mirror::Object> class_reference = h_class_reference->AllocObject(self);
  if (class_reference == nullptr) {
    if (access("/data/local/tmp/asx/ARGNULL", F_OK) == 0) {
      static thread_local int wl_kr = 0;
      if (wl_kr < 8) { wl_kr++;
        fprintf(stderr, "[WL-KREFL] getOrCreateKotlinClass fallback exit reason=4"
                        " -> deferring to the real method\n");
        fflush(stderr); }
    }
    return false;  // §667: do NOT claim the call; let real Kotlin reflection run.
  }
  ArtField* j_class_field =
      h_class_reference->FindInstanceField("jClass", "Ljava/lang/Class;");
  if (j_class_field == nullptr) {
    if (access("/data/local/tmp/asx/ARGNULL", F_OK) == 0) {
      static thread_local int wl_kr = 0;
      if (wl_kr < 8) { wl_kr++;
        fprintf(stderr, "[WL-KREFL] getOrCreateKotlinClass fallback exit reason=5"
                        " -> deferring to the real method\n");
        fflush(stderr); }
    }
    return false;  // §667: do NOT claim the call; let real Kotlin reflection run.
  }
  j_class_field->SetObject<false>(class_reference, h_java_class.Get());

  static thread_local int kotlin_reflection_fallback_count = 0;
  if (kotlin_reflection_fallback_count < 80) {
    kotlin_reflection_fallback_count++;
    fprintf(stderr,
            "[PFCUT] Kotlin reflection ClassReference fallback %s shorty=%s\n",
            called_method->PrettyMethod().c_str(),
            shorty);
    fflush(stderr);
  }

  result->SetL(class_reference);
  return true;
}

template <bool is_range>
static inline bool PFCutTryAndroidxLifecycleFactoryDefaultCreateBroadFallback(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs < 2u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Landroidx/lifecycle/ViewModelProvider$Factory;") ||
      strcmp(WlSafeName(called_method), "create") != 0) {
    return false;
  }

  const std::string pretty = called_method->PrettyMethod();
  if (pretty.find("androidx.lifecycle.viewmodel.CreationExtras") == std::string::npos ||
      pretty.find("java.lang.Class") == std::string::npos) {
    return false;
  }

  auto input_reg = [&](uint32_t input_index) -> uint32_t {
    return is_range ? vregC + input_index : arg[input_index];
  };

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(input_reg(0));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }

  ObjPtr<mirror::Object> java_class_object = nullptr;
  for (uint32_t i = 1; i < number_of_inputs; ++i) {
    ObjPtr<mirror::Object> candidate = shadow_frame.GetVRegReference(input_reg(i));
    if (candidate != nullptr && candidate->IsClass()) {
      java_class_object = candidate;
      break;
    }
  }
  if (java_class_object == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  ObjPtr<mirror::Class> requested_model_class = java_class_object->AsClass();
  if (requested_model_class == nullptr || !requested_model_class->IsInstantiable()) {
    result->SetL(nullptr);
    return true;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> h_requested_model_class(hs.NewHandle(requested_model_class));
  if (!class_linker->EnsureInitialized(self, h_requested_model_class, true, true)) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return true;
  }

  ArtMethod* init_method =
      h_requested_model_class->FindConstructor("()V", class_linker->GetImagePointerSize());
  Handle<mirror::Object> h_model(hs.NewHandle(h_requested_model_class->AllocObject(self)));
  if (self->IsExceptionPending()) {
    self->ClearException();
  }
  if (h_model == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  if (init_method != nullptr) {
    EnterInterpreterFromInvoke(self, init_method, h_model.Get(), nullptr, nullptr);
    if (self->IsExceptionPending()) {
      self->ClearException();
      result->SetL(nullptr);
      return true;
    }
  } else {
    const std::string model_descriptor = requested_model_class->PrettyDescriptor();
    if (model_descriptor.find("androidx.lifecycle.") != 0) {
      result->SetL(nullptr);
      return true;
    }
  }

  static thread_local int lifecycle_default_broad_count = 0;
  if (lifecycle_default_broad_count < 160 || WestlakeTraceVerboseCalls()) {
    lifecycle_default_broad_count++;
    fprintf(stderr,
            "[PFCUT] AndroidX ViewModelFactory default create broad fallback model %s inputs=%u\n",
            requested_model_class->PrettyDescriptor().c_str(),
            number_of_inputs);
    fflush(stderr);
  }
  result->SetL(h_model.Get());
  return true;
}

template <bool is_range>
static inline bool PFCutTryHiltViewModelFactoryCreateFallback(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      called_method->IsStatic() ||
      (number_of_inputs != 2u && number_of_inputs != 3u) ||
      called_method->GetDeclaringClass() == nullptr ||
      strcmp(WlSafeName(called_method), "create") != 0) {
    return false;
  }

  const std::string declaring_descriptor =
      called_method->GetDeclaringClass()->PrettyDescriptor();
  if (declaring_descriptor.find("dagger.hilt.android.internal.lifecycle.HiltViewModelFactory") ==
      std::string::npos) {
    return false;
  }

  auto input_reg = [&](uint32_t input_index) -> uint32_t {
    return is_range ? vregC + input_index : arg[input_index];
  };

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(input_reg(0));
  ObjPtr<mirror::Object> java_class_object = shadow_frame.GetVRegReference(input_reg(1));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }
  if (java_class_object == nullptr || !java_class_object->IsClass()) {
    result->SetL(nullptr);
    return true;
  }

  ObjPtr<mirror::Class> requested_model_class = java_class_object->AsClass();
  if (requested_model_class == nullptr || !requested_model_class->IsInstantiable()) {
    return false;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> h_requested_model_class(hs.NewHandle(requested_model_class));
  if (!class_linker->EnsureInitialized(self, h_requested_model_class, true, true)) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    return false;
  }

  ArtMethod* init_method =
      h_requested_model_class->FindConstructor("()V", class_linker->GetImagePointerSize());
  if (init_method == nullptr) {
    return false;
  }

  Handle<mirror::Object> h_model(hs.NewHandle(h_requested_model_class->AllocObject(self)));
  if (self->IsExceptionPending()) {
    self->ClearException();
  }
  if (h_model == nullptr) {
    return false;
  }

  EnterInterpreterFromInvoke(self, init_method, h_model.Get(), nullptr, nullptr);
  if (self->IsExceptionPending()) {
    self->ClearException();
    return false;
  }

  static thread_local int hilt_viewmodel_ctor_count = 0;
  if (hilt_viewmodel_ctor_count < 120 || WestlakeTraceVerboseCalls()) {
    hilt_viewmodel_ctor_count++;
    fprintf(stderr,
            "[PFCUT] Hilt ViewModelFactory create constructed model %s via %s\n",
            requested_model_class->PrettyDescriptor().c_str(),
            called_method->PrettyMethod().c_str());
    fflush(stderr);
  }
  result->SetL(h_model.Get());
  return true;
}

template <bool is_range>
static inline bool PFCutTryAndroidxLifecycleKClassFactoryFallback(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 3u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Landroidx/lifecycle/ViewModelProvider$Factory;") ||
      strcmp(WlSafeName(called_method), "create") != 0) {
    return false;
  }

  const std::string pretty = called_method->PrettyMethod();
  if (pretty.find("kotlin.reflect.KClass") == std::string::npos ||
      pretty.find("androidx.lifecycle.viewmodel.CreationExtras") == std::string::npos) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LLL") != 0) {
    return false;
  }

  auto input_reg = [&](uint32_t input_index) -> uint32_t {
    return is_range ? vregC + input_index : arg[input_index];
  };

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(input_reg(0));
  ObjPtr<mirror::Object> kclass = shadow_frame.GetVRegReference(input_reg(1));
  ObjPtr<mirror::Object> extras = shadow_frame.GetVRegReference(input_reg(2));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }
  if (kclass == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  ArtField* j_class_field =
      kclass->GetClass()->FindInstanceField("jClass", "Ljava/lang/Class;");
  if (j_class_field == nullptr) {
    return false;
  }
  ObjPtr<mirror::Object> java_class_object = j_class_field->GetObject(kclass);
  if (java_class_object == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  StackHandleScope<4> hs(self);
  Handle<mirror::Object> h_receiver(hs.NewHandle(receiver));
  Handle<mirror::Object> h_java_class_object(hs.NewHandle(java_class_object));
  Handle<mirror::Object> h_extras(hs.NewHandle(extras));
  Handle<mirror::Class> h_factory_class(hs.NewHandle(called_method->GetDeclaringClass()));

  const std::string receiver_descriptor = h_receiver->GetClass()->PrettyDescriptor();
  const bool direct_hilt_viewmodel_factory =
      receiver_descriptor.find("HiltViewModelFactory") != std::string::npos;
  if (direct_hilt_viewmodel_factory && h_java_class_object.Get()->IsClass()) {
    ObjPtr<mirror::Class> requested_model_class = h_java_class_object.Get()->AsClass();
    if (requested_model_class != nullptr &&
        requested_model_class->IsInstantiable() &&
        !requested_model_class->DescriptorEquals(
            "Ldagger/hilt/android/internal/managers/ActivityRetainedComponentManager$"
            "ActivityRetainedComponentViewModel;")) {
      ClassLinker* direct_class_linker = Runtime::Current()->GetClassLinker();
      Handle<mirror::Class> h_requested_model_class(hs.NewHandle(requested_model_class));
      if (direct_class_linker->EnsureInitialized(
              self, h_requested_model_class, true, true)) {
        ArtMethod* init_method = h_requested_model_class->FindConstructor(
            "()V", direct_class_linker->GetImagePointerSize());
        Handle<mirror::Object> h_fallback_model(
            hs.NewHandle(h_requested_model_class->AllocObject(self)));
        if (self->IsExceptionPending()) {
          self->ClearException();
        }
        if (init_method != nullptr && h_fallback_model != nullptr) {
          EnterInterpreterFromInvoke(self, init_method, h_fallback_model.Get(), nullptr, nullptr);
          if (self->IsExceptionPending()) {
            self->ClearException();
          } else {
            static thread_local int lifecycle_kclass_direct_alloc_count = 0;
            if (lifecycle_kclass_direct_alloc_count < 120 || WestlakeTraceVerboseCalls()) {
              lifecycle_kclass_direct_alloc_count++;
              fprintf(stderr,
                      "[PFCUT] AndroidX ViewModelFactory KClass create constructed model %s\n",
                      requested_model_class->PrettyDescriptor().c_str());
              fflush(stderr);
            }
            result->SetL(h_fallback_model.Get());
            return true;
          }
        }
      } else if (self->IsExceptionPending()) {
        self->ClearException();
      }
    }
  }

  static constexpr const char* kClassCreateSignature =
      "(Ljava/lang/Class;Landroidx/lifecycle/viewmodel/CreationExtras;)"
      "Landroidx/lifecycle/ViewModel;";
  ArtMethod* class_create_method =
      h_receiver->GetClass()->FindClassMethod("create",
                                              kClassCreateSignature,
                                              kRuntimePointerSize);
  const bool direct_receiver_method =
      class_create_method != nullptr && !class_create_method->IsAbstract();
  if (class_create_method == nullptr) {
    class_create_method =
        h_factory_class->FindInterfaceMethod("create",
                                             kClassCreateSignature,
                                             kRuntimePointerSize);
  }

  if (class_create_method != nullptr) {
    const std::string class_create_pretty = class_create_method->PrettyMethod();
    if (class_create_pretty.find(
            "androidx.lifecycle.ViewModelProvider$Factory.create") != std::string::npos &&
        h_java_class_object.Get()->IsClass()) {
      ObjPtr<mirror::Class> requested_model_class = h_java_class_object.Get()->AsClass();
      if (requested_model_class != nullptr && requested_model_class->IsInstantiable()) {
        ClassLinker* direct_class_linker = Runtime::Current()->GetClassLinker();
        Handle<mirror::Class> h_requested_model_class(hs.NewHandle(requested_model_class));
        if (direct_class_linker->EnsureInitialized(self, h_requested_model_class, true, true)) {
          ArtMethod* init_method = h_requested_model_class->FindConstructor(
              "()V", direct_class_linker->GetImagePointerSize());
          Handle<mirror::Object> h_fallback_model(
              hs.NewHandle(h_requested_model_class->AllocObject(self)));
          if (self->IsExceptionPending()) {
            self->ClearException();
          }
          if (h_fallback_model != nullptr) {
            bool model_ready = false;
            if (init_method != nullptr) {
              EnterInterpreterFromInvoke(self, init_method, h_fallback_model.Get(), nullptr, nullptr);
              if (self->IsExceptionPending()) {
                self->ClearException();
              } else {
                model_ready = true;
              }
            } else {
              const std::string model_descriptor =
                  h_requested_model_class->PrettyDescriptor();
              model_ready = model_descriptor.find("androidx.lifecycle.") == 0;
            }
            if (model_ready) {
              static thread_local int lifecycle_helper_default_synth_count = 0;
              if (lifecycle_helper_default_synth_count < 160 ||
                  WestlakeTraceVerboseCalls()) {
                lifecycle_helper_default_synth_count++;
                fprintf(stderr,
                        "[PFCUT] AndroidX ViewModelProviderImpl KClass helper synthesized "
                        "default Factory.create model %s\n",
                        h_requested_model_class->PrettyDescriptor().c_str());
                fflush(stderr);
              }
              result->SetL(h_fallback_model.Get());
              return true;
            }
          }
        } else if (self->IsExceptionPending()) {
          self->ClearException();
        }
      }
    }

    ScopedObjectAccessUnchecked soa(self);
    ScopedJniEnvLocalRefState env_state(soa.Env());
    jobject receiver_jobj = soa.AddLocalReference<jobject>(h_receiver.Get());
    jvalue create_args[2] = {};
    create_args[0].l = soa.AddLocalReference<jobject>(h_java_class_object.Get());
    create_args[1].l = soa.AddLocalReference<jobject>(h_extras.Get());

    JValue create_result = direct_receiver_method
        ? InvokeWithJValues(soa, receiver_jobj, class_create_method, create_args)
        : InvokeVirtualOrInterfaceWithJValues(soa, receiver_jobj, class_create_method, create_args);
    if (!self->IsExceptionPending() && create_result.GetL() != nullptr) {
      static thread_local int lifecycle_kclass_delegate_count = 0;
      if (lifecycle_kclass_delegate_count < 80) {
        lifecycle_kclass_delegate_count++;
        fprintf(stderr,
                "[PFCUT] AndroidX ViewModelFactory KClass create delegated to Class overload %s\n",
                class_create_method->PrettyMethod().c_str());
        fflush(stderr);
      }
      result->SetL(create_result.GetL());
      return true;
    }
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
  }

  if (h_java_class_object.Get()->IsClass()) {
    ObjPtr<mirror::Class> model_class = h_java_class_object.Get()->AsClass();
    if (model_class != nullptr && model_class->IsInstantiable()) {
      ObjPtr<mirror::Object> fallback_model = model_class->AllocObject(self);
      if (self->IsExceptionPending()) {
        self->ClearException();
      }
      if (fallback_model != nullptr) {
        static thread_local int lifecycle_kclass_alloc_count = 0;
        if (lifecycle_kclass_alloc_count < 80) {
          lifecycle_kclass_alloc_count++;
          fprintf(stderr,
                  "[PFCUT] AndroidX ViewModelFactory KClass create allocated model without ctor\n");
          fflush(stderr);
        }
        result->SetL(fallback_model);
        return true;
      }
    }
  }

  result->SetL(nullptr);
  return true;
}

template <bool is_range>
static inline bool PFCutTryAndroidxLifecycleKClassHelperFallback(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 3u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Landroidx/lifecycle/viewmodel/ViewModelProviderImpl_androidKt;")) {
    return false;
  }

  const std::string pretty = called_method->PrettyMethod();
  if (pretty.find("androidx.lifecycle.ViewModelProvider$Factory") == std::string::npos ||
      pretty.find("kotlin.reflect.KClass") == std::string::npos ||
      pretty.find("androidx.lifecycle.viewmodel.CreationExtras") == std::string::npos) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LLLL") != 0) {
    return false;
  }

  auto input_reg = [&](uint32_t input_index) -> uint32_t {
    return is_range ? vregC + input_index : arg[input_index];
  };

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(input_reg(0));
  ObjPtr<mirror::Object> kclass = shadow_frame.GetVRegReference(input_reg(1));
  ObjPtr<mirror::Object> extras = shadow_frame.GetVRegReference(input_reg(2));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }
  if (kclass == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  ArtField* j_class_field =
      kclass->GetClass()->FindInstanceField("jClass", "Ljava/lang/Class;");
  if (j_class_field == nullptr) {
    return false;
  }
  ObjPtr<mirror::Object> java_class_object = j_class_field->GetObject(kclass);
  if (java_class_object == nullptr) {
    result->SetL(nullptr);
    return true;
  }

  StackHandleScope<7> hs(self);
  Handle<mirror::Object> h_receiver(hs.NewHandle(receiver));
  Handle<mirror::Object> h_java_class_object(hs.NewHandle(java_class_object));
  Handle<mirror::Object> h_extras(hs.NewHandle(extras));

  const std::string receiver_descriptor = h_receiver->GetClass()->PrettyDescriptor();
  const bool direct_hilt_viewmodel_factory =
      receiver_descriptor.find("HiltViewModelFactory") != std::string::npos;
  if (direct_hilt_viewmodel_factory && h_java_class_object.Get()->IsClass()) {
    ObjPtr<mirror::Class> requested_model_class = h_java_class_object.Get()->AsClass();
    if (requested_model_class != nullptr &&
        requested_model_class->IsInstantiable() &&
        !requested_model_class->DescriptorEquals(
            "Ldagger/hilt/android/internal/managers/ActivityRetainedComponentManager$"
            "ActivityRetainedComponentViewModel;")) {
      ClassLinker* direct_class_linker = Runtime::Current()->GetClassLinker();
      Handle<mirror::Class> h_requested_model_class(hs.NewHandle(requested_model_class));
      if (direct_class_linker->EnsureInitialized(
              self, h_requested_model_class, true, true)) {
        ArtMethod* init_method = h_requested_model_class->FindConstructor(
            "()V", direct_class_linker->GetImagePointerSize());
        Handle<mirror::Object> h_fallback_model(
            hs.NewHandle(h_requested_model_class->AllocObject(self)));
        if (self->IsExceptionPending()) {
          self->ClearException();
        }
        if (init_method != nullptr && h_fallback_model != nullptr) {
          EnterInterpreterFromInvoke(self, init_method, h_fallback_model.Get(), nullptr, nullptr);
          if (self->IsExceptionPending()) {
            self->ClearException();
          } else {
            static thread_local int lifecycle_helper_direct_alloc_count = 0;
            if (lifecycle_helper_direct_alloc_count < 120 || WestlakeTraceVerboseCalls()) {
              lifecycle_helper_direct_alloc_count++;
              fprintf(stderr,
                      "[PFCUT] AndroidX ViewModelProviderImpl KClass helper constructed model %s\n",
                      requested_model_class->PrettyDescriptor().c_str());
              fflush(stderr);
            }
            result->SetL(h_fallback_model.Get());
            return true;
          }
        }
      } else if (self->IsExceptionPending()) {
        self->ClearException();
      }
    }
  }

  Handle<mirror::ClassLoader> h_loader(
      hs.NewHandle(called_method->GetDeclaringClass()->GetClassLoader()));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> h_factory_class(hs.NewHandle(
      class_linker->FindClass(self, "Landroidx/lifecycle/ViewModelProvider$Factory;", h_loader)));
  if (h_factory_class == nullptr && self->IsExceptionPending()) {
    self->ClearException();
  }

  static constexpr const char* kClassCreateWithExtrasSignature =
      "(Ljava/lang/Class;Landroidx/lifecycle/viewmodel/CreationExtras;)"
      "Landroidx/lifecycle/ViewModel;";
  ArtMethod* class_create_method =
      h_receiver->GetClass()->FindClassMethod("create",
                                              kClassCreateWithExtrasSignature,
                                              kRuntimePointerSize);
  bool direct_receiver_method =
      class_create_method != nullptr && !class_create_method->IsAbstract();
  if (class_create_method == nullptr && h_factory_class != nullptr) {
    class_create_method =
        h_factory_class->FindInterfaceMethod("create",
                                             kClassCreateWithExtrasSignature,
                                             kRuntimePointerSize);
    direct_receiver_method = false;
  }

  if (class_create_method != nullptr) {
    const std::string class_create_pretty = class_create_method->PrettyMethod();
    if (class_create_pretty.find(
            "androidx.lifecycle.ViewModelProvider$Factory.create") != std::string::npos &&
        h_java_class_object.Get()->IsClass()) {
      ObjPtr<mirror::Class> requested_model_class = h_java_class_object.Get()->AsClass();
      if (requested_model_class != nullptr && requested_model_class->IsInstantiable()) {
        ClassLinker* direct_class_linker = Runtime::Current()->GetClassLinker();
        Handle<mirror::Class> h_requested_model_class(hs.NewHandle(requested_model_class));
        if (direct_class_linker->EnsureInitialized(self, h_requested_model_class, true, true)) {
          ArtMethod* init_method = h_requested_model_class->FindConstructor(
              "()V", direct_class_linker->GetImagePointerSize());
          Handle<mirror::Object> h_fallback_model(
              hs.NewHandle(h_requested_model_class->AllocObject(self)));
          if (self->IsExceptionPending()) {
            self->ClearException();
          }
          if (h_fallback_model != nullptr) {
            bool model_ready = false;
            if (init_method != nullptr) {
              EnterInterpreterFromInvoke(self, init_method, h_fallback_model.Get(), nullptr, nullptr);
              if (self->IsExceptionPending()) {
                self->ClearException();
              } else {
                model_ready = true;
              }
            } else {
              const std::string model_descriptor =
                  h_requested_model_class->PrettyDescriptor();
              model_ready = model_descriptor.find("androidx.lifecycle.") == 0;
            }
            if (model_ready) {
              static thread_local int lifecycle_helper_default_synth_count = 0;
              if (lifecycle_helper_default_synth_count < 160 ||
                  WestlakeTraceVerboseCalls()) {
                lifecycle_helper_default_synth_count++;
                fprintf(stderr,
                        "[PFCUT] AndroidX ViewModelProviderImpl KClass helper synthesized "
                        "default Factory.create model %s\n",
                        h_requested_model_class->PrettyDescriptor().c_str());
                fflush(stderr);
              }
              result->SetL(h_fallback_model.Get());
              return true;
            }
          }
        } else if (self->IsExceptionPending()) {
          self->ClearException();
        }
      }
    }

    ScopedObjectAccessUnchecked soa(self);
    ScopedJniEnvLocalRefState env_state(soa.Env());
    jobject receiver_jobj = soa.AddLocalReference<jobject>(h_receiver.Get());
    jvalue create_args[2] = {};
    create_args[0].l = soa.AddLocalReference<jobject>(h_java_class_object.Get());
    create_args[1].l = soa.AddLocalReference<jobject>(h_extras.Get());

    JValue create_result = direct_receiver_method
        ? InvokeWithJValues(soa, receiver_jobj, class_create_method, create_args)
        : InvokeVirtualOrInterfaceWithJValues(soa, receiver_jobj, class_create_method, create_args);
    if (!self->IsExceptionPending() && create_result.GetL() != nullptr) {
      static thread_local int lifecycle_helper_delegate_count = 0;
      if (lifecycle_helper_delegate_count < 80) {
        lifecycle_helper_delegate_count++;
        fprintf(stderr,
                "[PFCUT] AndroidX ViewModelProviderImpl KClass helper delegated to Class+Extras overload %s\n",
                class_create_method->PrettyMethod().c_str());
        fflush(stderr);
      }
      result->SetL(create_result.GetL());
      return true;
    }
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
  }

  static constexpr const char* kClassCreateSignature =
      "(Ljava/lang/Class;)Landroidx/lifecycle/ViewModel;";
  class_create_method =
      h_receiver->GetClass()->FindClassMethod("create",
                                              kClassCreateSignature,
                                              kRuntimePointerSize);
  direct_receiver_method =
      class_create_method != nullptr && !class_create_method->IsAbstract();
  if (class_create_method == nullptr && h_factory_class != nullptr) {
    class_create_method =
        h_factory_class->FindInterfaceMethod("create",
                                             kClassCreateSignature,
                                             kRuntimePointerSize);
    direct_receiver_method = false;
  }

  if (class_create_method != nullptr) {
    ScopedObjectAccessUnchecked soa(self);
    ScopedJniEnvLocalRefState env_state(soa.Env());
    jobject receiver_jobj = soa.AddLocalReference<jobject>(h_receiver.Get());
    jvalue create_args[1] = {};
    create_args[0].l = soa.AddLocalReference<jobject>(h_java_class_object.Get());

    JValue create_result = direct_receiver_method
        ? InvokeWithJValues(soa, receiver_jobj, class_create_method, create_args)
        : InvokeVirtualOrInterfaceWithJValues(soa, receiver_jobj, class_create_method, create_args);
    if (!self->IsExceptionPending() && create_result.GetL() != nullptr) {
      static thread_local int lifecycle_helper_delegate_legacy_count = 0;
      if (lifecycle_helper_delegate_legacy_count < 80) {
        lifecycle_helper_delegate_legacy_count++;
        fprintf(stderr,
                "[PFCUT] AndroidX ViewModelProviderImpl KClass helper delegated to Class overload %s\n",
                class_create_method->PrettyMethod().c_str());
        fflush(stderr);
      }
      result->SetL(create_result.GetL());
      return true;
    }
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
  }

  if (h_java_class_object.Get()->IsClass()) {
    ObjPtr<mirror::Class> model_class = h_java_class_object.Get()->AsClass();
    if (model_class != nullptr && model_class->IsInstantiable()) {
      ObjPtr<mirror::Object> fallback_model = model_class->AllocObject(self);
      if (self->IsExceptionPending()) {
        self->ClearException();
      }
      if (fallback_model != nullptr) {
        static thread_local int lifecycle_helper_alloc_count = 0;
        if (lifecycle_helper_alloc_count < 80) {
          lifecycle_helper_alloc_count++;
          fprintf(stderr,
                  "[PFCUT] AndroidX ViewModelProviderImpl KClass helper allocated model without ctor\n");
          fflush(stderr);
        }
        result->SetL(fallback_model);
        return true;
      }
    }
  }

  result->SetL(nullptr);
  return true;
}

template <bool is_range>
static inline bool PFCutTryAndroidxLifecycleClassFactoryFallback(
    ArtMethod* called_method,
    Thread* self,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      self == nullptr ||
      called_method->IsStatic() ||
      (number_of_inputs != 2u && number_of_inputs != 3u) ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Landroidx/lifecycle/ViewModelProvider$Factory;") ||
      strcmp(WlSafeName(called_method), "create") != 0) {
    return false;
  }

  const std::string pretty = called_method->PrettyMethod();
  const bool has_extras =
      pretty.find("androidx.lifecycle.viewmodel.CreationExtras") != std::string::npos;
  if (number_of_inputs == 3u && !has_extras) {
    return false;
  }
  if (pretty.find("java.lang.Class") == std::string::npos) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr ||
      ((number_of_inputs == 2u && strcmp(shorty, "LL") != 0) ||
       (number_of_inputs == 3u && strcmp(shorty, "LLL") != 0))) {
    return false;
  }

  auto input_reg = [&](uint32_t input_index) -> uint32_t {
    return is_range ? vregC + input_index : arg[input_index];
  };

  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(input_reg(0));
  ObjPtr<mirror::Object> java_class_object = shadow_frame.GetVRegReference(input_reg(1));
  ObjPtr<mirror::Object> extras =
      number_of_inputs == 3u ? shadow_frame.GetVRegReference(input_reg(2)) : nullptr;
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    result->SetJ(0);
    return true;
  }
  if (java_class_object == nullptr || !java_class_object->IsClass()) {
    result->SetL(nullptr);
    return true;
  }

  StackHandleScope<3> hs(self);
  Handle<mirror::Object> h_receiver(hs.NewHandle(receiver));
  Handle<mirror::Object> h_java_class_object(hs.NewHandle(java_class_object));
  Handle<mirror::Object> h_extras(hs.NewHandle(extras));

  ObjPtr<mirror::Class> requested_model_class = h_java_class_object.Get()->AsClass();
  if (requested_model_class != nullptr && requested_model_class->IsInstantiable()) {
    ObjPtr<mirror::Object> fallback_model = requested_model_class->AllocObject(self);
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    if (fallback_model != nullptr) {
      static thread_local int lifecycle_class_direct_alloc_count = 0;
      if (lifecycle_class_direct_alloc_count < 120 || WestlakeTraceVerboseCalls()) {
        lifecycle_class_direct_alloc_count++;
        fprintf(stderr,
                "[PFCUT] AndroidX ViewModelFactory Class create direct allocated model without ctor %s\n",
                requested_model_class->PrettyDescriptor().c_str());
        fflush(stderr);
      }
      result->SetL(fallback_model);
      return true;
    }
  }

  auto try_invoke_receiver = [&](const char* signature,
                                 bool pass_extras,
                                 const char* label) -> ObjPtr<mirror::Object> {
    ArtMethod* receiver_method =
        h_receiver->GetClass()->FindClassMethod("create", signature, kRuntimePointerSize);
    if (receiver_method == nullptr ||
        receiver_method == called_method ||
        receiver_method->IsAbstract() ||
        receiver_method->GetDeclaringClass() == called_method->GetDeclaringClass()) {
      return nullptr;
    }

    ScopedObjectAccessUnchecked soa(self);
    ScopedJniEnvLocalRefState env_state(soa.Env());
    jobject receiver_jobj = soa.AddLocalReference<jobject>(h_receiver.Get());
    jvalue create_args[2] = {};
    create_args[0].l = soa.AddLocalReference<jobject>(h_java_class_object.Get());
    if (pass_extras) {
      create_args[1].l = soa.AddLocalReference<jobject>(h_extras.Get());
    }

    JValue create_result = InvokeWithJValues(soa, receiver_jobj, receiver_method, create_args);
    if (!self->IsExceptionPending() && create_result.GetL() != nullptr) {
      static thread_local int lifecycle_class_delegate_count = 0;
      if (lifecycle_class_delegate_count < 80) {
        lifecycle_class_delegate_count++;
        fprintf(stderr,
                "[PFCUT] AndroidX ViewModelFactory Class create delegated %s via %s\n",
                receiver_method->PrettyMethod().c_str(),
                label);
        fflush(stderr);
      }
      return create_result.GetL();
    }
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    return nullptr;
  };

  static constexpr const char* kClassCreateWithExtrasSignature =
      "(Ljava/lang/Class;Landroidx/lifecycle/viewmodel/CreationExtras;)"
      "Landroidx/lifecycle/ViewModel;";
  static constexpr const char* kClassCreateSignature =
      "(Ljava/lang/Class;)Landroidx/lifecycle/ViewModel;";

  ObjPtr<mirror::Object> created = nullptr;
  if (number_of_inputs == 3u) {
    created = try_invoke_receiver(kClassCreateWithExtrasSignature, true, "Class+Extras");
  }
  if (created == nullptr) {
    created = try_invoke_receiver(kClassCreateSignature, false, "Class");
  }
  if (created != nullptr) {
    result->SetL(created);
    return true;
  }

  ObjPtr<mirror::Class> model_class = java_class_object->AsClass();
  if (model_class != nullptr && model_class->IsInstantiable()) {
    ObjPtr<mirror::Object> fallback_model = model_class->AllocObject(self);
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    if (fallback_model != nullptr) {
      static thread_local int lifecycle_class_alloc_count = 0;
      if (lifecycle_class_alloc_count < 80) {
        lifecycle_class_alloc_count++;
        fprintf(stderr,
                "[PFCUT] AndroidX ViewModelFactory Class create allocated model without ctor %s\n",
                model_class->PrettyDescriptor().c_str());
        fflush(stderr);
      }
      result->SetL(fallback_model);
      return true;
    }
  }

  result->SetL(nullptr);
  return true;
}

static inline void PFCutSetDefaultResultForShorty(const char* shorty, JValue* result) {
  if (shorty == nullptr || shorty[0] == '\0' || result == nullptr) {
    return;
  }

  switch (shorty[0]) {
    case 'V':
      return;
    case 'Z':
      result->SetZ(false);
      return;
    case 'B':
    case 'C':
    case 'S':
    case 'I':
      result->SetI(0);
      return;
    case 'J':
      result->SetJ(0);
      return;
    case 'F':
      result->SetF(0.0f);
      return;
    case 'D':
      result->SetD(0.0);
      return;
    case 'L':
    case '[':
      result->SetL(nullptr);
      return;
    default:
      result->SetJ(0);
      return;
  }
}

static inline int64_t PFCutNextRealmPseudoHandle() {
  static std::atomic<int64_t> next_handle(0x570000000000LL);
  return next_handle.fetch_add(0x10, std::memory_order_relaxed);
}

struct PFCutRealmPropertyState {
  std::string table;
  std::string name;
  int64_t column_key = 0;
  int32_t type = 0;

  // Keep the weak move-assignment symbol stable across diagnostic-only build
  // variants.  The quiet compiler otherwise elides its sole out-of-line use,
  // producing an unnecessary dynsym difference from the deployed baseline.
  __attribute__((used)) PFCutRealmPropertyState& operator=(PFCutRealmPropertyState&&) = default;
};

struct PFCutRealmTableState {
  std::string name;
};

struct PFCutRealmSchemaState {
  std::string class_name;
};

struct PFCutRealmQueryState {
  int64_t table_handle = 0;
  std::string table;
  std::string predicate;
  std::vector<int64_t> bind_handles;
  std::vector<std::string> bind_strings;
  std::vector<int64_t> bind_longs;
  std::vector<int64_t> row_ids;
};

struct PFCutRealmResultState {
  int64_t table_handle = 0;
  int64_t query_handle = 0;
  std::string table;
  std::vector<int64_t> row_ids;
};

struct PFCutRealmRowState {
  std::string table;
  int64_t row_id = -1;
  std::unordered_map<std::string, std::string> strings;
  std::unordered_map<std::string, int64_t> longs;
  std::unordered_map<std::string, std::vector<int64_t>> link_lists;
};

struct PFCutRealmAnyState {
  std::string kind;
  std::string string_value;
  int64_t long_value = 0;
  bool bool_value = false;
};

struct PFCutRealmBuilderState {
  std::unordered_map<std::string, std::string> strings;
  std::unordered_map<std::string, int64_t> longs;
  std::unordered_map<std::string, std::vector<int64_t>> link_lists;
};

struct PFCutRealmListState {
  std::string table;
  int64_t owner_row_id = -1;
  std::string column;
  std::string target_table;
  std::vector<int64_t> row_ids;
};

struct PFCutRealmSharedRealmState {
  int32_t owner_tid = 0;
  uint32_t close_count = 0;
  bool closed = false;
};

struct PFCutRealmState {
  std::mutex mu;
  int64_t next_row_id = 1;
  std::unordered_map<int64_t, PFCutRealmPropertyState> properties;
  std::unordered_map<int64_t, PFCutRealmTableState> tables;
  std::unordered_map<int64_t, PFCutRealmSchemaState> schemas;
  std::unordered_map<int64_t, PFCutRealmQueryState> queries;
  std::unordered_map<int64_t, PFCutRealmResultState> results;
  std::unordered_map<int64_t, PFCutRealmRowState> rows;
  std::unordered_map<int64_t, PFCutRealmAnyState> native_any;
  std::unordered_map<int64_t, PFCutRealmBuilderState> builders;
  std::unordered_map<int64_t, PFCutRealmListState> lists;
  std::unordered_map<std::string, PFCutRealmRowState> row_templates;
  std::unordered_map<std::string, int64_t> table_handles;
  std::unordered_map<std::string, int64_t> schema_handles;
  std::unordered_map<std::string, int64_t> column_keys;
  std::unordered_map<int64_t, std::string> column_names;
  std::unordered_set<int64_t> open_transactions;
  std::unordered_set<int64_t> closed_realms;
  std::unordered_map<int64_t, PFCutRealmSharedRealmState> shared_realms;
  std::unordered_set<int64_t> released_handles;
};

static PFCutRealmState& PFCutRealmGlobalState() {
  static PFCutRealmState* state = new PFCutRealmState();
  return *state;
}

static inline int32_t PFCutRealmCurrentTid() {
  Thread* self = Thread::Current();
  return self != nullptr ? static_cast<int32_t>(self->GetTid()) : 0;
}

static const char* PFCutRealmHandleKindLocked(PFCutRealmState& state, int64_t handle) {
  if (handle == 0) {
    return "zero";
  }
  if (state.shared_realms.find(handle) != state.shared_realms.end()) {
    return "shared-realm";
  }
  if (state.tables.find(handle) != state.tables.end()) {
    return "table";
  }
  if (state.schemas.find(handle) != state.schemas.end()) {
    return "schema";
  }
  if (state.properties.find(handle) != state.properties.end()) {
    return "property";
  }
  if (state.queries.find(handle) != state.queries.end()) {
    return "query";
  }
  if (state.results.find(handle) != state.results.end()) {
    return "result";
  }
  if (state.rows.find(handle) != state.rows.end()) {
    return "row";
  }
  if (state.native_any.find(handle) != state.native_any.end()) {
    return "native-any";
  }
  if (state.builders.find(handle) != state.builders.end()) {
    return "builder";
  }
  if (state.lists.find(handle) != state.lists.end()) {
    return "list";
  }
  return "unknown";
}

static void PFCutRealmReleaseHandleLocked(PFCutRealmState& state,
                                          int64_t native_ptr,
                                          const char** out_kind,
                                          bool* out_first_release) {
  const char* kind = PFCutRealmHandleKindLocked(state, native_ptr);
  bool first_release = false;
  if (native_ptr != 0) {
    first_release = state.released_handles.insert(native_ptr).second;
    auto realm_it = state.shared_realms.find(native_ptr);
    if (realm_it != state.shared_realms.end()) {
      state.open_transactions.erase(native_ptr);
      state.closed_realms.insert(native_ptr);
      realm_it->second.closed = true;
    }
  }
  if (out_kind != nullptr) {
    *out_kind = kind;
  }
  if (out_first_release != nullptr) {
    *out_first_release = first_release;
  }
}

static inline std::string PFCutRealmStringParam(ObjPtr<mirror::Object> object)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (object == nullptr || !object->IsString()) {
    return std::string();
  }
  return object->AsString()->ToModifiedUtf8();
}

static inline bool PFCutRealmParamReg(const char* shorty,
                                      bool is_static,
                                      uint16_t number_of_inputs,
                                      uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                      uint32_t vregC,
                                      uint32_t param_index,
                                      bool is_range,
                                      uint32_t* out_reg) {
  if (shorty == nullptr || out_reg == nullptr) {
    return false;
  }
  uint32_t input_slot = is_static ? 0u : 1u;
  uint32_t current_param = 0u;
  for (uint32_t shorty_index = 1; shorty[shorty_index] != '\0'; ++shorty_index) {
    const char kind = shorty[shorty_index];
    if (input_slot >= number_of_inputs) {
      return false;
    }
    const uint32_t reg = is_range ? (vregC + input_slot) : arg[input_slot];
    if (current_param == param_index) {
      *out_reg = reg;
      return true;
    }
    input_slot += (kind == 'J' || kind == 'D') ? 2u : 1u;
    current_param++;
  }
  return false;
}

template <bool is_range>
static inline std::string PFCutRealmGetStringParam(ArtMethod* called_method,
                                                   const char* shorty,
                                                   ShadowFrame& shadow_frame,
                                                   uint16_t number_of_inputs,
                                                   uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                                   uint32_t vregC,
                                                   uint32_t param_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  uint32_t reg = 0;
  if (!PFCutRealmParamReg(shorty,
                         called_method->IsStatic(),
                         number_of_inputs,
                         arg,
                         vregC,
                         param_index,
                         is_range,
                         &reg)) {
    return std::string();
  }
  return PFCutRealmStringParam(shadow_frame.GetVRegReference(reg));
}

template <bool is_range>
static inline int64_t PFCutRealmGetLongParam(ArtMethod* called_method,
                                             const char* shorty,
                                             ShadowFrame& shadow_frame,
                                             uint16_t number_of_inputs,
                                             uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                             uint32_t vregC,
                                             uint32_t param_index,
                                             int64_t fallback = 0)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  uint32_t reg = 0;
  if (!PFCutRealmParamReg(shorty,
                         called_method->IsStatic(),
                         number_of_inputs,
                         arg,
                         vregC,
                         param_index,
                         is_range,
                         &reg)) {
    return fallback;
  }
  return shadow_frame.GetVRegLong(reg);
}

template <bool is_range>
static inline int32_t PFCutRealmGetIntParam(ArtMethod* called_method,
                                            const char* shorty,
                                            ShadowFrame& shadow_frame,
                                            uint16_t number_of_inputs,
                                            uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                            uint32_t vregC,
                                            uint32_t param_index,
                                            int32_t fallback = 0)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  uint32_t reg = 0;
  if (!PFCutRealmParamReg(shorty,
                         called_method->IsStatic(),
                         number_of_inputs,
                         arg,
                         vregC,
                         param_index,
                         is_range,
                         &reg)) {
    return fallback;
  }
  return static_cast<int32_t>(shadow_frame.GetVReg(reg));
}

template <bool is_range>
static inline bool PFCutRealmGetBoolParam(ArtMethod* called_method,
                                          const char* shorty,
                                          ShadowFrame& shadow_frame,
                                          uint16_t number_of_inputs,
                                          uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                          uint32_t vregC,
                                          uint32_t param_index,
                                          bool fallback = false)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  uint32_t reg = 0;
  if (!PFCutRealmParamReg(shorty,
                         called_method->IsStatic(),
                         number_of_inputs,
                         arg,
                         vregC,
                         param_index,
                         is_range,
                         &reg)) {
    return fallback;
  }
  return shadow_frame.GetVReg(reg) != 0;
}

template <bool is_range>
static inline std::vector<int64_t> PFCutRealmGetLongArrayParam(
    ArtMethod* called_method,
    const char* shorty,
    ShadowFrame& shadow_frame,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC,
    uint32_t param_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  uint32_t reg = 0;
  if (!PFCutRealmParamReg(shorty,
                         called_method->IsStatic(),
                         number_of_inputs,
                         arg,
                         vregC,
                         param_index,
                         is_range,
                         &reg)) {
    return {};
  }
  ObjPtr<mirror::Object> object = shadow_frame.GetVRegReference(reg);
  if (object == nullptr || !object->IsLongArray()) {
    return {};
  }
  ObjPtr<mirror::LongArray> array = object->AsLongArray();
  std::vector<int64_t> values;
  values.reserve(static_cast<size_t>(array->GetLength()));
  for (int32_t i = 0; i < array->GetLength(); ++i) {
    values.push_back(array->GetWithoutChecks(i));
  }
  return values;
}

static inline std::string PFCutRealmNormalizeTableName(const std::string& raw) {
  if (raw == "KeyValueStore" || raw == "class_KeyValueStore") {
    return "class_KeyValueStore";
  }
  if (raw == "BaseCart" || raw == "class_BaseCart") {
    return "class_BaseCart";
  }
  if (raw == "CartProduct" || raw == "class_CartProduct") {
    return "class_CartProduct";
  }
  if (raw == "Configuration" || raw == "class_Configuration") {
    return "class_Configuration";
  }
  return raw;
}

static inline std::string PFCutRealmClassNameFromTable(const std::string& table) {
  if (table.compare(0, 6, "class_") == 0) {
    return table.substr(6);
  }
  return table;
}

static inline std::string PFCutRealmColumnMapKey(const std::string& table,
                                                 const std::string& column) {
  return PFCutRealmNormalizeTableName(table) + "." + column;
}

static inline bool PFCutRealmIsTrackedTable(const std::string& table) {
  const std::string normalized = PFCutRealmNormalizeTableName(table);
  return normalized == "class_KeyValueStore" ||
      normalized == "class_BaseCart" ||
      normalized == "class_CartProduct" ||
      normalized == "class_Configuration";
}

static int64_t PFCutRealmStableColumnKeyLocked(PFCutRealmState& state,
                                               const std::string& table,
                                               const std::string& column) {
  const std::string normalized_table = PFCutRealmNormalizeTableName(table);
  const std::string key = PFCutRealmColumnMapKey(normalized_table, column);
  auto it = state.column_keys.find(key);
  if (it != state.column_keys.end()) {
    return it->second;
  }
  const int64_t value = 0x4d430000LL +
      static_cast<int64_t>(state.column_keys.size() + 1u);
  state.column_keys[key] = value;
  state.column_names[value] = column;
  fprintf(stderr,
          "[PFCUT-REALM-STATE] column table=%s name=%s key=%lld\n",
          normalized_table.c_str(),
          column.c_str(),
          static_cast<long long>(value));
  fflush(stderr);
  return value;
}

static int64_t PFCutRealmTableHandleLocked(PFCutRealmState& state,
                                           const std::string& table) {
  const std::string normalized = PFCutRealmNormalizeTableName(table);
  auto it = state.table_handles.find(normalized);
  if (it != state.table_handles.end()) {
    return it->second;
  }
  const int64_t handle = PFCutNextRealmPseudoHandle();
  state.table_handles[normalized] = handle;
  state.tables[handle] = {normalized};
  fprintf(stderr,
          "[PFCUT-REALM-STATE] table name=%s handle=%lld tracked=%d\n",
          normalized.c_str(),
          static_cast<long long>(handle),
          PFCutRealmIsTrackedTable(normalized) ? 1 : 0);
  fflush(stderr);
  return handle;
}

static int64_t PFCutRealmSchemaHandleLocked(PFCutRealmState& state,
                                            const std::string& class_name) {
  const std::string table = PFCutRealmNormalizeTableName(class_name);
  const std::string normalized_class = PFCutRealmClassNameFromTable(table);
  auto it = state.schema_handles.find(normalized_class);
  if (it != state.schema_handles.end()) {
    return it->second;
  }
  const int64_t handle = PFCutNextRealmPseudoHandle();
  state.schema_handles[normalized_class] = handle;
  state.schemas[handle] = {normalized_class};
  fprintf(stderr,
          "[PFCUT-REALM-STATE] schema class=%s handle=%lld\n",
          normalized_class.c_str(),
          static_cast<long long>(handle));
  fflush(stderr);
  return handle;
}

static int64_t PFCutRealmPropertyHandleLocked(PFCutRealmState& state,
                                              const std::string& table,
                                              const std::string& column,
                                              int32_t type) {
  const int64_t column_key = PFCutRealmStableColumnKeyLocked(state, table, column);
  const int64_t handle = PFCutNextRealmPseudoHandle();
  state.properties[handle] = {PFCutRealmNormalizeTableName(table), column, column_key, type};
  fprintf(stderr,
          "[PFCUT-REALM-STATE] property table=%s name=%s handle=%lld columnKey=%lld type=%d\n",
          PFCutRealmNormalizeTableName(table).c_str(),
          column.c_str(),
          static_cast<long long>(handle),
          static_cast<long long>(column_key),
          type);
  fflush(stderr);
  return handle;
}

static std::string PFCutRealmColumnNameLocked(PFCutRealmState& state, int64_t column_key) {
  auto it = state.column_names.find(column_key);
  return it != state.column_names.end() ? it->second : std::string();
}

static int64_t PFCutRealmResultSizeLocked(const PFCutRealmResultState& result_state) {
  return static_cast<int64_t>(result_state.row_ids.size());
}

static std::string PFCutRealmRowTemplateKey(const std::string& table, int64_t row_id) {
  return PFCutRealmNormalizeTableName(table) + "#" + std::to_string(row_id);
}

static std::string PFCutRealmColumnAlias(const std::string& column) {
  if (column == "_createdOn") {
    return "createdOn";
  }
  if (column == "_maxAge") {
    return "maxAge";
  }
  return column;
}

static std::string PFCutRealmLinkListTargetTable(const std::string& owner_table,
                                                 const std::string& column) {
  const std::string normalized_column = PFCutRealmColumnAlias(column);
  (void)owner_table;
  if (normalized_column == "cartProducts" ||
      normalized_column == "components" ||
      normalized_column == "choices" ||
      normalized_column == "customizations" ||
      normalized_column == "selectedChoices") {
    return "class_CartProduct";
  }
  return "";
}

static bool PFCutRealmSeededKeyValueValue(const std::string& key, std::string* out) {
  if (out == nullptr) {
    return false;
  }
  if (key == "language" || key == "languageName") {
    *out = "en-US";
    return true;
  }
  if (key == "country" || key == "market") {
    *out = "US";
    return true;
  }
  if (key == "marketId") {
    *out = "usdap_prod";
    return true;
  }
  if (key == "currentAppVersion" || key == "applicationVersion") {
    *out = "7.0.0";
    return true;
  }
  if (key == "currentAppVersionCode") {
    *out = "700000";
    return true;
  }
  if (key == "deviceToken") {
    *out = "westlake-device-token";
    return true;
  }
  if (key == "SERVER_AUTH_TOKEN" || key == "serverAuthToken") {
    *out = "westlake-offline-auth-token";
    return true;
  }
  if (key == "SELECTED_CONFIG" || key == "selectedConfig") {
    *out =
        "{\"marketId\":\"usdap_prod\",\"country\":\"US\",\"language\":\"en-US\","
        "\"currencyCode\":\"USD\",\"currencySymbol\":\"$\","
        "\"justFlip.splitEnvironmentId\":\"474f8810-15c1-11ee-b490-4afd76afc5e7\","
        "\"justFlip.splitApiKey\":\"offline\","
        "\"baseUrl\":\"https://us-prod.api.mcd.com/exp/v1/\","
        "\"tokenUrl\":\"https://us-prod.api.mcd.com/v1/security/auth/token\","
        "\"configMap\":{\"justFlip.splitEnvironmentId\":"
        "\"474f8810-15c1-11ee-b490-4afd76afc5e7\"}}";
    return true;
  }
  if (key == "homeDashboardSections") {
    *out =
        "[{\"Enabled\":true,\"Name\":\"HERO\"},"
        "{\"Enabled\":true,\"Name\":\"MENU\"},"
        "{\"Enabled\":true,\"Name\":\"DEALS\"},"
        "{\"Enabled\":true,\"Name\":\"PROMOTION\"}]";
    return true;
  }
  if (key == "user_interface.order.menu") {
    *out = "[]";
    return true;
  }
  if (key == "AppFeature.AppParameter") {
    *out =
        "{\"BOTH\":{\"currentOperationMode\":\"BOTH\","
        "\"operationModes\":[{\"name\":\"BOTH\",\"enabled\":true}]},"
        "\"US\":{\"currentOperationMode\":\"BOTH\","
        "\"operationModes\":[{\"name\":\"BOTH\",\"enabled\":true}]},"
        "\"usdap_prod\":{\"currentOperationMode\":\"BOTH\","
        "\"operationModes\":[{\"name\":\"BOTH\",\"enabled\":true}]}}";
    return true;
  }
  return false;
}

static std::string PFCutRealmSeededServerConfigJson() {
  return
      "{\"marketId\":\"usdap_prod\",\"country\":\"US\",\"language\":\"en-US\","
      "\"currencyCode\":\"USD\",\"currencySymbol\":\"$\","
      "\"homeDashboardSections\":["
      "{\"Enabled\":true,\"Name\":\"HERO\"},"
      "{\"Enabled\":true,\"Name\":\"MENU\"},"
      "{\"Enabled\":true,\"Name\":\"DEALS\"},"
      "{\"Enabled\":true,\"Name\":\"PROMOTION\"}],"
      "\"user_interface\":{\"order\":{\"menu\":[]}},"
      "\"AppFeature\":{\"AppParameter\":{\"BOTH\":{\"currentOperationMode\":\"BOTH\","
      "\"operationModes\":[{\"name\":\"BOTH\",\"enabled\":true}]}}},"
      "\"justFlip.splitEnvironmentId\":\"474f8810-15c1-11ee-b490-4afd76afc5e7\","
      "\"justFlip.splitApiKey\":\"offline\","
      "\"baseUrl\":\"https://us-prod.api.mcd.com/exp/v1/\","
      "\"tokenUrl\":\"https://us-prod.api.mcd.com/v1/security/auth/token\"}";
}

static int64_t PFCutRealmEnsureKeyValueRowLocked(PFCutRealmState& state,
                                                 const std::string& key,
                                                 const std::string& value) {
  const std::string table = "class_KeyValueStore";
  for (const auto& entry : state.row_templates) {
    if (entry.second.table != table) {
      continue;
    }
    const auto key_it = entry.second.strings.find("key");
    if (key_it != entry.second.strings.end() && key_it->second == key) {
      return entry.second.row_id;
    }
  }

  const int64_t row_id = state.next_row_id++;
  PFCutRealmRowState row;
  row.table = table;
  row.row_id = row_id;
  row.longs["createdOn"] = 0;
  row.longs["maxAge"] = -1;
  row.strings["key"] = key;
  row.strings["value"] = value;
  state.row_templates[PFCutRealmRowTemplateKey(table, row_id)] = row;
  fprintf(stderr,
          "[PFCUT-REALM-STATE] keyvalue-row key=%s row=%lld valueLen=%zu\n",
          key.c_str(),
          static_cast<long long>(row_id),
          value.size());
  fflush(stderr);
  return row_id;
}

static int64_t PFCutRealmEnsureConfigurationRowLocked(PFCutRealmState& state,
                                                      const std::string& name) {
  const std::string table = "class_Configuration";
  for (const auto& entry : state.row_templates) {
    if (entry.second.table != table) {
      continue;
    }
    const auto name_it = entry.second.strings.find("name");
    if (name_it != entry.second.strings.end() && name_it->second == name) {
      return entry.second.row_id;
    }
  }

  const int64_t row_id = state.next_row_id++;
  PFCutRealmRowState row;
  row.table = table;
  row.row_id = row_id;
  row.longs["createdOn"] = 0;
  row.longs["maxAge"] = -1;
  row.longs["configVersion"] = 1;
  row.strings["name"] = name;
  row.strings["config"] = PFCutRealmSeededServerConfigJson();
  row.strings["configId"] = "westlake-usdap-prod";
  row.strings["configVersion"] = "1";
  row.strings["modifiedDateTime"] = "2026-04-30T00:00:00Z";
  state.row_templates[PFCutRealmRowTemplateKey(table, row_id)] = row;
  fprintf(stderr,
          "[PFCUT-REALM-STATE] configuration-row name=%s row=%lld configLen=%zu\n",
          name.c_str(),
          static_cast<long long>(row_id),
          row.strings["config"].size());
  fflush(stderr);
  return row_id;
}

static PFCutRealmRowState& PFCutRealmEnsureRowTemplateLocked(PFCutRealmState& state,
                                                             const std::string& table,
                                                             int64_t row_id) {
  std::string key = PFCutRealmRowTemplateKey(table, row_id);
  auto it = state.row_templates.find(key);
  if (it == state.row_templates.end()) {
    PFCutRealmRowState row;
    row.table = PFCutRealmNormalizeTableName(table);
    row.row_id = row_id;
    it = state.row_templates.emplace(key, std::move(row)).first;
  }
  return it->second;
}

static int64_t PFCutRealmCreateRowTemplateLocked(PFCutRealmState& state,
                                                 const std::string& table) {
  const int64_t row_id = state.next_row_id++;
  PFCutRealmEnsureRowTemplateLocked(state, table, row_id);
  return row_id;
}

static void PFCutRealmStoreLongLocked(PFCutRealmState& state,
                                      const std::string& table,
                                      int64_t row_id,
                                      const std::string& column,
                                      int64_t value) {
  PFCutRealmRowState& row = PFCutRealmEnsureRowTemplateLocked(state, table, row_id);
  row.longs[PFCutRealmColumnAlias(column)] = value;
}

static void PFCutRealmStoreStringLocked(PFCutRealmState& state,
                                        const std::string& table,
                                        int64_t row_id,
                                        const std::string& column,
                                        const std::string& value) {
  PFCutRealmRowState& row = PFCutRealmEnsureRowTemplateLocked(state, table, row_id);
  row.strings[PFCutRealmColumnAlias(column)] = value;
}

static int64_t PFCutRealmEnsureBaseCartRowLocked(PFCutRealmState& state) {
  const std::string table = "class_BaseCart";
  const int64_t basecart_row_id = 0;
  PFCutRealmRowState& row = PFCutRealmEnsureRowTemplateLocked(state, table, basecart_row_id);
  if (row.longs.find("cartStatus") != row.longs.end() ||
      row.strings.find("cartUuid") != row.strings.end() ||
      row.link_lists.find("cartProducts") != row.link_lists.end()) {
    return basecart_row_id;
  }
  const int64_t row_id = basecart_row_id;
  PFCutRealmStoreLongLocked(state, table, row_id, "createdOn", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "maxAge", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "cartStatus", 1);
  PFCutRealmStoreLongLocked(state, table, row_id, "orderValue", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "totalValue", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "totalDue", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "totalTax", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "totalDiscount", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "totalEnergy", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "deliveryFee", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "confirmationNeeded", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "isEdtCalculationEnabled", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "isLargeOrder", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "isPaidOrder", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "isSpotNumberRequired", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "isTpOrder", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "lastValidatedTime", 0);
  PFCutRealmStoreLongLocked(state, table, row_id, "resultCode", 0);
  PFCutRealmStoreStringLocked(state, table, row_id, "cartUuid", "westlake-cart");
  PFCutRealmStoreStringLocked(state, table, row_id, "storeId", "westlake-store");
  PFCutRealmStoreStringLocked(state, table, row_id, "marketId", "us");
  PFCutRealmStoreStringLocked(state, table, row_id, "languageName", "en-US");
  PFCutRealmStoreStringLocked(state, table, row_id, "nickName", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "orderPaymentId", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "productionResponse", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "estimatedDeliveryTime", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "orderDate", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "orderId", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "orderNumber", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "orderStatus", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "tenderType", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "randomCode", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "barCode", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "checkInCode", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "orderTinData", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "validationType", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "operationMode", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "options", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "priceType", "");
  PFCutRealmStoreStringLocked(state, table, row_id, "offersValidation", "");
  fprintf(stderr, "[PFCUT-REALM-WRITE] basecart-seed row=%lld reason=ensure\n",
          static_cast<long long>(row_id));
  fflush(stderr);
  return row_id;
}

static void PFCutRealmApplyBuilderLocked(PFCutRealmState& state,
                                         const std::string& table,
                                         int64_t row_id,
                                         int64_t builder_handle) {
  const auto builder_it = state.builders.find(builder_handle);
  if (builder_it == state.builders.end()) {
    return;
  }
  PFCutRealmRowState& row = PFCutRealmEnsureRowTemplateLocked(state, table, row_id);
  for (const auto& entry : builder_it->second.longs) {
    row.longs[PFCutRealmColumnAlias(entry.first)] = entry.second;
  }
  for (const auto& entry : builder_it->second.strings) {
    row.strings[PFCutRealmColumnAlias(entry.first)] = entry.second;
  }
  for (const auto& entry : builder_it->second.link_lists) {
    row.link_lists[PFCutRealmColumnAlias(entry.first)] = entry.second;
    fprintf(stderr,
            "[PFCUT-REALM-WRITE] builder-commit-list table=%s row=%lld column=%s count=%zu\n",
            PFCutRealmNormalizeTableName(table).c_str(),
            static_cast<long long>(row_id),
            PFCutRealmColumnAlias(entry.first).c_str(),
            entry.second.size());
    fflush(stderr);
  }
}

static void PFCutRealmDecodeQueryBindsLocked(PFCutRealmState& state,
                                             PFCutRealmQueryState* query) {
  if (query == nullptr) {
    return;
  }
  query->bind_strings.clear();
  query->bind_longs.clear();
  for (int64_t handle : query->bind_handles) {
    auto it = state.native_any.find(handle);
    if (it == state.native_any.end()) {
      continue;
    }
    if (it->second.kind == "string") {
      query->bind_strings.push_back(it->second.string_value);
    } else if (it->second.kind == "long" || it->second.kind == "date") {
      query->bind_longs.push_back(it->second.long_value);
    } else if (it->second.kind == "bool") {
      query->bind_longs.push_back(it->second.bool_value ? 1 : 0);
    }
  }
}

static void PFCutRealmMaybePopulateQueryRowsLocked(PFCutRealmState& state,
                                                   PFCutRealmQueryState* query) {
  if (query == nullptr) {
    return;
  }
  query->row_ids.clear();
  const std::string table = PFCutRealmNormalizeTableName(query->table);
  const std::string bind0 = query->bind_strings.empty() ? "" : query->bind_strings.front();
  const int64_t bind0l = query->bind_longs.empty() ? 0 : query->bind_longs.front();
  if (!bind0.empty() &&
      table == "class_KeyValueStore" &&
      query->predicate.find("key = $0") != std::string::npos) {
    std::string value;
    if (!PFCutRealmSeededKeyValueValue(bind0, &value)) {
      return;
    }
    query->row_ids.push_back(PFCutRealmEnsureKeyValueRowLocked(state, bind0, value));
    return;
  }
  if (!bind0.empty() &&
      table == "class_Configuration" &&
      (query->predicate.find("name =[c] $0") != std::string::npos ||
       query->predicate.find("name = $0") != std::string::npos) &&
      bind0 == "serverConfig") {
    query->row_ids.push_back(PFCutRealmEnsureConfigurationRowLocked(state, bind0));
    return;
  }
  if (table == "class_BaseCart") {
    const bool active_cart_predicate =
        query->predicate.empty() ||
        query->predicate.find("cartStatus = $0") != std::string::npos ||
        query->predicate.find("_maxAge < $0") != std::string::npos ||
        query->predicate.find("maxAge < $0") != std::string::npos ||
        query->predicate.find("_maxAge != $0") != std::string::npos ||
        query->predicate.find("maxAge != $0") != std::string::npos;
    if (active_cart_predicate) {
      const int64_t row_id = PFCutRealmEnsureBaseCartRowLocked(state);
      query->row_ids.push_back(row_id);
      fprintf(stderr,
              "[PFCUT-REALM-STATE] basecart-active-query row=%lld predicate=%s bind0=%lld\n",
              static_cast<long long>(row_id),
              query->predicate.c_str(),
              static_cast<long long>(bind0l));
      fflush(stderr);
    }
  }
}

static PFCutRealmRowState PFCutRealmRowFromTemplateLocked(PFCutRealmState& state,
                                                          const std::string& table,
                                                          int64_t row_id) {
  const auto it = state.row_templates.find(PFCutRealmRowTemplateKey(table, row_id));
  if (it != state.row_templates.end()) {
    return it->second;
  }
  PFCutRealmRowState row;
  row.table = PFCutRealmNormalizeTableName(table);
  row.row_id = row_id;
  return row;
}

static int64_t PFCutRealmCreateRowHandleLocked(PFCutRealmState& state,
                                               const std::string& table,
                                               int64_t row_id) {
  if (row_id < 0) {
    return 0;
  }
  const int64_t row_handle = PFCutNextRealmPseudoHandle();
  state.rows[row_handle] = PFCutRealmRowFromTemplateLocked(state, table, row_id);
  return row_handle;
}

static std::vector<std::string> PFCutRealmKnownColumnsForTable(const std::string& table) {
  const std::string normalized = PFCutRealmNormalizeTableName(table);
  if (normalized == "class_KeyValueStore") {
    return {"createdOn", "maxAge", "key", "value"};
  }
  if (normalized == "class_BaseCart") {
    return {
        "createdOn",
        "maxAge",
        "cartUuid",
        "cartStatus",
        "storeId",
        "nickName",
        "orderPaymentId",
        "deliveryFee",
        "productionResponse",
        "confirmationNeeded",
        "estimatedDeliveryTime",
        "isEdtCalculationEnabled",
        "isLargeOrder",
        "isPaidOrder",
        "isSpotNumberRequired",
        "marketId",
        "languageName",
        "orderDate",
        "orderId",
        "orderNumber",
        "orderStatus",
        "orderValue",
        "tenderType",
        "totalValue",
        "totalDue",
        "totalTax",
        "totalDiscount",
        "totalEnergy",
        "randomCode",
        "barCode",
        "checkInCode",
        "orderTinData",
        "payments",
        "estimatedInStoreDeliveryTime",
        "cartOffers",
        "cartPromotions",
        "cartProducts",
        "deposits",
        "fees",
        "cumulatedTaxInfo",
        "savings",
        "validationType",
        "operationMode",
        "options",
        "isTpOrder",
        "priceType",
        "lastValidatedTime",
        "resultCode",
        "offersValidation"};
  }
  if (normalized == "class_CartProduct") {
    return {
        "createdOn",
        "maxAge",
        "productCode",
        "quantity",
        "productType",
        "isCustom",
        "basePrice",
        "totalPrice"};
  }
  if (normalized == "class_Configuration") {
    return {"createdOn",
            "maxAge",
            "name",
            "config",
            "configId",
            "configVersion",
            "modifiedDateTime"};
  }
  return {};
}

static std::string PFCutRealmTableForHandleLocked(PFCutRealmState& state, int64_t handle) {
  auto table_it = state.tables.find(handle);
  if (table_it != state.tables.end()) {
    return table_it->second.name;
  }
  auto query_it = state.queries.find(handle);
  if (query_it != state.queries.end()) {
    return query_it->second.table;
  }
  auto result_it = state.results.find(handle);
  if (result_it != state.results.end()) {
    return result_it->second.table;
  }
  auto row_it = state.rows.find(handle);
  if (row_it != state.rows.end()) {
    return row_it->second.table;
  }
  return std::string();
}

static bool PFCutRealmSetStringResult(Thread* self,
                                      const std::string& value,
                                      JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::String> str = mirror::String::AllocFromModifiedUtf8(self, value.c_str());
  if (self->IsExceptionPending()) {
    self->ClearException();
    return false;
  }
  result->SetL(str);
  return true;
}

static bool PFCutRealmSetStringArrayResult(Thread* self,
                                           const std::vector<std::string>& values,
                                           JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::ObjectArray<mirror::String>> h_array(hs.NewHandle(
      mirror::ObjectArray<mirror::String>::Alloc(
          self,
          GetClassRoot<mirror::ObjectArray<mirror::String>>(),
          static_cast<int32_t>(values.size()))));
  if (h_array == nullptr || self->IsExceptionPending()) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return false;
  }
  for (int32_t i = 0; i < static_cast<int32_t>(values.size()); ++i) {
    ObjPtr<mirror::String> entry =
        mirror::String::AllocFromModifiedUtf8(self, values[i].c_str());
    if (entry == nullptr || self->IsExceptionPending()) {
      if (self->IsExceptionPending()) {
        self->ClearException();
      }
      result->SetL(nullptr);
      return false;
    }
    h_array->SetWithoutChecks</*kTransactionActive=*/ false,
                               /*kCheckTransaction=*/ false>(i, entry);
  }
  result->SetL(h_array.Get());
  return true;
}

static bool PFCutRealmSetLongPairResult(Thread* self,
                                        int64_t first,
                                        int64_t second,
                                        JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::LongArray> array = mirror::LongArray::Alloc(self, 2);
  if (array == nullptr || self->IsExceptionPending()) {
    if (self->IsExceptionPending()) {
      self->ClearException();
    }
    result->SetL(nullptr);
    return false;
  }
  array->Set(0, first);
  array->Set(1, second);
  result->SetL(array);
  return true;
}

template <bool is_range>
static bool PFCutTryRealmNativeState(ArtMethod* called_method,
                                     const char* descriptor,
                                     const char* shorty,
                                     ShadowFrame& shadow_frame,
                                     JValue* result,
                                     uint16_t number_of_inputs,
                                     uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                     uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || descriptor == nullptr || shorty == nullptr || result == nullptr) {
    return false;
  }
  const char* method_name = WlSafeName(called_method);
  if (method_name == nullptr) {
    return false;
  }

  Thread* self = Thread::Current();
  PFCutRealmState& state = PFCutRealmGlobalState();

  if (strcmp(descriptor, "Lio/realm/internal/NativeObjectReference;") == 0) {
    if (strcmp(method_name, "nativeCleanUp") == 0) {
      const int64_t finalizer_ptr = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t native_ptr = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      const char* kind = "unknown";
      bool first_release = false;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        PFCutRealmReleaseHandleLocked(state, native_ptr, &kind, &first_release);
      }
      fprintf(stderr,
              "[PFCUT-REALM-FINALIZER] cleanup finalizer=%lld handle=%lld kind=%s first=%d\n",
              static_cast<long long>(finalizer_ptr),
              static_cast<long long>(native_ptr),
              kind,
              first_release ? 1 : 0);
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/core/NativeRealmAny;") == 0) {
    if (strcmp(method_name, "nativeCreateString") == 0 ||
        strcmp(method_name, "nativeCreateObjectId") == 0 ||
        strcmp(method_name, "nativeCreateUUID") == 0) {
      const std::string value = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u);
      const int64_t handle = PFCutNextRealmPseudoHandle();
      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.native_any[handle] = {"string", value, 0, false};
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] native-any-create method=%s handle=%lld kind=string value=%s\n",
              method_name,
              static_cast<long long>(handle),
              value.c_str());
      fflush(stderr);
      result->SetJ(handle);
      return true;
    }
    if (strcmp(method_name, "nativeCreateLong") == 0 ||
        strcmp(method_name, "nativeCreateDate") == 0) {
      const int64_t value = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t handle = PFCutNextRealmPseudoHandle();
      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.native_any[handle] = {
            strcmp(method_name, "nativeCreateDate") == 0 ? "date" : "long", "", value, false};
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] native-any-create method=%s handle=%lld kind=long value=%lld\n",
              method_name,
              static_cast<long long>(handle),
              static_cast<long long>(value));
      fflush(stderr);
      result->SetJ(handle);
      return true;
    }
    if (strcmp(method_name, "nativeCreateBoolean") == 0) {
      const bool value = PFCutRealmGetBoolParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, false);
      const int64_t handle = PFCutNextRealmPseudoHandle();
      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.native_any[handle] = {"bool", "", 0, value};
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] native-any-create method=%s handle=%lld kind=bool value=%d\n",
              method_name,
              static_cast<long long>(handle),
              value ? 1 : 0);
      fflush(stderr);
      result->SetJ(handle);
      return true;
    }
    if (strcmp(method_name, "nativeCreateNull") == 0) {
      const int64_t handle = PFCutNextRealmPseudoHandle();
      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.native_any[handle] = {"null", "", 0, false};
      }
      result->SetJ(handle);
      return true;
    }
    if (strcmp(method_name, "nativeAsString") == 0 ||
        strcmp(method_name, "nativeAsObjectId") == 0 ||
        strcmp(method_name, "nativeAsUUID") == 0 ||
        strcmp(method_name, "nativeGetRealmModelTableName") == 0) {
      const int64_t handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      std::string value;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.native_any.find(handle);
        if (it != state.native_any.end() && it->second.kind == "string") {
          value = it->second.string_value;
        }
      }
      return PFCutRealmSetStringResult(self, value, result);
    }
    if (strcmp(method_name, "nativeAsLong") == 0 ||
        strcmp(method_name, "nativeAsDate") == 0 ||
        strcmp(method_name, "nativeGetRealmModelRowKey") == 0) {
      const int64_t handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t value = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.native_any.find(handle);
        if (it != state.native_any.end()) {
          value = it->second.long_value;
        }
      }
      result->SetJ(value);
      return true;
    }
    if (strcmp(method_name, "nativeAsBoolean") == 0) {
      const int64_t handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      bool value = false;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.native_any.find(handle);
        if (it != state.native_any.end()) {
          value = it->second.bool_value;
        }
      }
      result->SetZ(value);
      return true;
    }
    if (strcmp(method_name, "nativeEquals") == 0) {
      const int64_t lhs = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t rhs = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      result->SetZ(lhs == rhs);
      return true;
    }
    if (strcmp(method_name, "nativeGetType") == 0) {
      const int64_t handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int32_t type = 11;  // RealmAny.Type.NULL ordinal in the Java SDK shape used by McD.
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.native_any.find(handle);
        if (it != state.native_any.end()) {
          if (it->second.kind == "long") {
            type = 0;
          } else if (it->second.kind == "bool") {
            type = 1;
          } else if (it->second.kind == "string") {
            type = 2;
          } else if (it->second.kind == "date") {
            type = 4;
          }
        }
      }
      result->SetI(type);
      return true;
    }
    if (shorty[0] == 'J') {
      result->SetJ(0);
      return true;
    }
    if (shorty[0] == 'Z') {
      result->SetZ(false);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/Property;") == 0) {
    if (strcmp(method_name, "nativeCreatePersistedProperty") == 0 ||
        strcmp(method_name, "nativeCreatePersistedLinkProperty") == 0 ||
        strcmp(method_name, "nativeCreateComputedLinkProperty") == 0) {
      const std::string column = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u);
      const int32_t type = PFCutRealmGetIntParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, 0);
      std::lock_guard<std::mutex> lock(state.mu);
      const int64_t handle = PFCutNextRealmPseudoHandle();
      state.properties[handle] = {"", column, 0, type};
      fprintf(stderr,
              "[PFCUT-REALM-STATE] property-create name=%s handle=%lld type=%d\n",
              column.c_str(),
              static_cast<long long>(handle),
              type);
      fflush(stderr);
      result->SetJ(handle);
      return true;
    }
    if (strcmp(method_name, "nativeGetColumnKey") == 0) {
      const int64_t property_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t column_key = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        auto it = state.properties.find(property_handle);
        if (it != state.properties.end()) {
          if (it->second.column_key == 0) {
            it->second.column_key =
                PFCutRealmStableColumnKeyLocked(state, it->second.table, it->second.name);
          }
          column_key = it->second.column_key;
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] property-column-key property=%lld key=%lld\n",
              static_cast<long long>(property_handle),
              static_cast<long long>(column_key));
      fflush(stderr);
      result->SetJ(column_key);
      return true;
    }
    if (strcmp(method_name, "nativeGetType") == 0) {
      result->SetI(0);
      return true;
    }
    if (strcmp(method_name, "nativeGetLinkedObjectName") == 0) {
      return PFCutRealmSetStringResult(self, "", result);
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/OsObjectSchemaInfo;") == 0) {
    if (strcmp(method_name, "nativeCreateRealmObjectSchema") == 0) {
      std::string class_name = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u);
      if (class_name.empty()) {
        class_name = PFCutRealmGetStringParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u);
      }
      int64_t schema_handle = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        schema_handle = PFCutRealmSchemaHandleLocked(state, class_name);
        PFCutRealmTableHandleLocked(state, class_name);
      }
      result->SetJ(schema_handle);
      return true;
    }
    if (strcmp(method_name, "nativeAddProperties") == 0) {
      const int64_t schema_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      std::lock_guard<std::mutex> lock(state.mu);
      const auto schema_it = state.schemas.find(schema_handle);
      fprintf(stderr,
              "[PFCUT-REALM-STATE] schema-add-properties schema=%lld class=%s\n",
              static_cast<long long>(schema_handle),
              schema_it != state.schemas.end() ? schema_it->second.class_name.c_str() : "");
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (strcmp(method_name, "nativeGetProperty") == 0) {
      const int64_t schema_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const std::string column = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u);
      int64_t property_handle = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        std::string table;
        const auto schema_it = state.schemas.find(schema_handle);
        if (schema_it != state.schemas.end()) {
          table = PFCutRealmNormalizeTableName(schema_it->second.class_name);
        }
        if (table.empty()) {
          table = "class_KeyValueStore";
        }
        property_handle = PFCutRealmPropertyHandleLocked(state, table, column, 0);
      }
      result->SetJ(property_handle);
      return true;
    }
    if (strcmp(method_name, "nativeGetClassName") == 0) {
      const int64_t schema_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      std::string class_name;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto schema_it = state.schemas.find(schema_handle);
        if (schema_it != state.schemas.end()) {
          class_name = schema_it->second.class_name;
        }
      }
      return PFCutRealmSetStringResult(self, class_name, result);
    }
    if (strcmp(method_name, "nativeGetPrimaryKeyProperty") == 0) {
      result->SetJ(0);
      return true;
    }
    if (strcmp(method_name, "nativeIsEmbedded") == 0) {
      result->SetZ(false);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/OsSchemaInfo;") == 0) {
    if (strcmp(method_name, "nativeCreateFromList") == 0) {
      const int64_t handle = PFCutNextRealmPseudoHandle();
      fprintf(stderr,
              "[PFCUT-REALM-STATE] schema-list handle=%lld\n",
              static_cast<long long>(handle));
      fflush(stderr);
      result->SetJ(handle);
      return true;
    }
    if (strcmp(method_name, "nativeGetObjectSchemaInfo") == 0) {
      const std::string class_name = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u);
      int64_t schema_handle = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        schema_handle = PFCutRealmSchemaHandleLocked(state, class_name);
        PFCutRealmTableHandleLocked(state, class_name);
      }
      result->SetJ(schema_handle);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/OsRealmConfig;") == 0) {
    if (strcmp(method_name, "nativeCreate") == 0) {
      const int64_t handle = PFCutNextRealmPseudoHandle();
      fprintf(stderr,
              "[PFCUT-REALM-STATE] config handle=%lld\n",
              static_cast<long long>(handle));
      fflush(stderr);
      result->SetJ(handle);
      return true;
    }
    if (strstr(method_name, "nativeSet") == method_name ||
        strcmp(method_name, "nativeEnableChangeNotification") == 0) {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/OsSharedRealm;") == 0) {
    const bool returns_shared_realm = strcmp(method_name, "nativeGetSharedRealm") == 0 ||
        strcmp(method_name, "nativeFreeze") == 0;
    if (returns_shared_realm ||
        strcmp(method_name, "nativeGetSchemaInfo") == 0 ||
        strcmp(method_name, "nativeGetActiveSubscriptionSet") == 0 ||
        strcmp(method_name, "nativeGetLatestSubscriptionSet") == 0) {
      const int64_t handle = PFCutNextRealmPseudoHandle();
      int32_t owner_tid = 0;
      if (returns_shared_realm) {
        owner_tid = PFCutRealmCurrentTid();
        std::lock_guard<std::mutex> lock(state.mu);
        state.shared_realms[handle] = {owner_tid, 0, false};
        state.closed_realms.erase(handle);
        state.released_handles.erase(handle);
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] shared-realm-handle method=%s handle=%lld ownerTid=%d tracked=%d\n",
              method_name,
              static_cast<long long>(handle),
              owner_tid,
              returns_shared_realm ? 1 : 0);
      fflush(stderr);
      result->SetJ(handle);
      return true;
    }
    if (strcmp(method_name, "nativeGetTableRef") == 0 ||
        strcmp(method_name, "nativeCreateTable") == 0 ||
        strcmp(method_name, "nativeCreateTableWithPrimaryKeyField") == 0) {
      const std::string table = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u);
      int64_t table_handle = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        table_handle = PFCutRealmTableHandleLocked(state, table);
      }
      result->SetJ(table_handle);
      return true;
    }
    if (strcmp(method_name, "nativeHasTable") == 0) {
      const std::string table = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u);
      result->SetZ(PFCutRealmIsTrackedTable(table));
      return true;
    }
    if (strcmp(method_name, "nativeGetTablesName") == 0) {
      return PFCutRealmSetStringArrayResult(
          self,
          {"class_KeyValueStore", "class_BaseCart", "class_CartProduct", "class_Configuration"},
          result);
    }
    if (strcmp(method_name, "nativeGetVersionID") == 0) {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (strcmp(method_name, "nativeBeginTransaction") == 0) {
      const int64_t realm_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int32_t owner_tid = 0;
      bool closed = realm_handle == 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        if (realm_handle != 0) {
          PFCutRealmSharedRealmState& realm = state.shared_realms[realm_handle];
          if (realm.owner_tid == 0) {
            realm.owner_tid = PFCutRealmCurrentTid();
          }
          owner_tid = realm.owner_tid;
          closed = realm.closed ||
              state.closed_realms.find(realm_handle) != state.closed_realms.end() ||
              state.released_handles.find(realm_handle) != state.released_handles.end();
          if (!closed) {
            state.open_transactions.insert(realm_handle);
          }
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] transaction-begin realm=%lld ownerTid=%d closed=%d\n",
              static_cast<long long>(realm_handle),
              owner_tid,
              closed ? 1 : 0);
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (strcmp(method_name, "nativeCommitTransaction") == 0 ||
        strcmp(method_name, "nativeCancelTransaction") == 0) {
      const int64_t realm_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      bool was_open = false;
      bool closed = realm_handle == 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        was_open = state.open_transactions.erase(realm_handle) > 0;
        auto realm_it = state.shared_realms.find(realm_handle);
        if (realm_it != state.shared_realms.end()) {
          closed = realm_it->second.closed;
        }
        closed = closed ||
            state.closed_realms.find(realm_handle) != state.closed_realms.end() ||
            state.released_handles.find(realm_handle) != state.released_handles.end();
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] transaction-%s realm=%lld wasOpen=%d closed=%d\n",
              strcmp(method_name, "nativeCommitTransaction") == 0 ? "commit" : "cancel",
              static_cast<long long>(realm_handle),
              was_open ? 1 : 0,
              closed ? 1 : 0);
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (strcmp(method_name, "nativeCloseSharedRealm") == 0) {
      const int64_t realm_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int32_t tid = PFCutRealmCurrentTid();
      int32_t owner_tid = 0;
      uint32_t close_count = 0;
      bool already_closed = realm_handle == 0;
      bool first_release = false;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        if (realm_handle != 0) {
          PFCutRealmSharedRealmState& realm = state.shared_realms[realm_handle];
          if (realm.owner_tid == 0) {
            realm.owner_tid = tid;
          }
          owner_tid = realm.owner_tid;
          already_closed = realm.closed ||
              state.closed_realms.find(realm_handle) != state.closed_realms.end() ||
              state.released_handles.find(realm_handle) != state.released_handles.end();
          realm.closed = true;
          realm.close_count++;
          close_count = realm.close_count;
          state.open_transactions.erase(realm_handle);
          state.closed_realms.insert(realm_handle);
          first_release = state.released_handles.insert(realm_handle).second;
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-CLOSE] realm=%lld tid=%d ownerTid=%d alreadyClosed=%d closeCount=%u firstRelease=%d\n",
              static_cast<long long>(realm_handle),
              tid,
              owner_tid,
              already_closed ? 1 : 0,
              close_count,
              first_release ? 1 : 0);
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (strcmp(method_name, "nativeIsInTransaction") == 0 ||
        strcmp(method_name, "nativeIsClosed") == 0) {
      const int64_t realm_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      bool value = false;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        bool closed = realm_handle == 0;
        auto realm_it = state.shared_realms.find(realm_handle);
        if (realm_it != state.shared_realms.end()) {
          closed = closed || realm_it->second.closed;
        }
        closed = closed ||
            state.closed_realms.find(realm_handle) != state.closed_realms.end() ||
            state.released_handles.find(realm_handle) != state.released_handles.end();
        value = strcmp(method_name, "nativeIsInTransaction") == 0
            ? (!closed && state.open_transactions.find(realm_handle) != state.open_transactions.end())
            : closed;
      }
      result->SetZ(value);
      return true;
    }
    if (shorty[0] == 'Z') {
      result->SetZ(false);
      return true;
    }
    if (shorty[0] == 'J') {
      result->SetJ(0);
      return true;
    }
    if (shorty[0] == 'V') {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/Table;") == 0) {
    if (strcmp(method_name, "nativeGetName") == 0) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      std::string table;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        table = PFCutRealmTableForHandleLocked(state, table_handle);
      }
      return PFCutRealmSetStringResult(self, table, result);
    }
    if (strcmp(method_name, "nativeGetColumnKey") == 0) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const std::string column = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u);
      int64_t column_key = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        std::string table = PFCutRealmTableForHandleLocked(state, table_handle);
        column_key = PFCutRealmStableColumnKeyLocked(state, table, column);
      }
      result->SetJ(column_key);
      return true;
    }
    if (strcmp(method_name, "nativeGetColumnName") == 0) {
      const int64_t column_key = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      std::string column;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        column = PFCutRealmColumnNameLocked(state, column_key);
      }
      return PFCutRealmSetStringResult(self, column, result);
    }
    if (strcmp(method_name, "nativeGetColumnNames") == 0) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      std::vector<std::string> columns;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        columns = PFCutRealmKnownColumnsForTable(PFCutRealmTableForHandleLocked(state, table_handle));
      }
      return PFCutRealmSetStringArrayResult(self, columns, result);
    }
    if (strcmp(method_name, "nativeGetColumnCount") == 0) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t count = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        count = static_cast<int64_t>(
            PFCutRealmKnownColumnsForTable(PFCutRealmTableForHandleLocked(state, table_handle)).size());
      }
      result->SetJ(count);
      return true;
    }
    if (strcmp(method_name, "nativeGetColumnType") == 0) {
      result->SetI(0);
      return true;
    }
    if (strcmp(method_name, "nativeWhere") == 0) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t query_handle = PFCutNextRealmPseudoHandle();
      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.queries[query_handle] =
            {table_handle, PFCutRealmTableForHandleLocked(state, table_handle), ""};
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] query tableHandle=%lld query=%lld\n",
              static_cast<long long>(table_handle),
              static_cast<long long>(query_handle));
      fflush(stderr);
      result->SetJ(query_handle);
      return true;
    }
    if (strcmp(method_name, "nativeGetRowPtr") == 0) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t row_id = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, -1);
      int64_t row_handle = 0;
      std::string table;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        table = PFCutRealmTableForHandleLocked(state, table_handle);
        row_handle = PFCutRealmCreateRowHandleLocked(state, table, row_id);
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] table-row table=%s row=%lld handle=%lld\n",
              table.c_str(),
              static_cast<long long>(row_id),
              static_cast<long long>(row_handle));
      fflush(stderr);
      result->SetJ(row_handle);
      return true;
    }
    if (strcmp(method_name, "nativeSize") == 0) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t size = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const std::string table = PFCutRealmTableForHandleLocked(state, table_handle);
        for (const auto& entry : state.row_templates) {
          if (entry.second.table == table) {
            size++;
          }
        }
      }
      result->SetJ(size);
      return true;
    }
    if (strstr(method_name, "nativeSet") == method_name) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t column_key = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      const int64_t row_id = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, -1);
      std::string table;
      std::string column;
      int64_t long_value = 0;
      std::string string_value;
      if (strcmp(method_name, "nativeSetString") == 0 ||
          strcmp(method_name, "nativeSetObjectId") == 0 ||
          strcmp(method_name, "nativeSetUUID") == 0) {
        string_value = PFCutRealmGetStringParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 3u);
      } else if (strcmp(method_name, "nativeSetBoolean") == 0) {
        long_value = PFCutRealmGetBoolParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 3u, false) ? 1 : 0;
      } else if (strcmp(method_name, "nativeSetNull") != 0) {
        long_value = PFCutRealmGetLongParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 3u, 0);
      }
      {
        std::lock_guard<std::mutex> lock(state.mu);
        table = PFCutRealmTableForHandleLocked(state, table_handle);
        column = PFCutRealmColumnAlias(PFCutRealmColumnNameLocked(state, column_key));
        if (strcmp(method_name, "nativeSetNull") == 0) {
          PFCutRealmRowState& row = PFCutRealmEnsureRowTemplateLocked(state, table, row_id);
          row.longs.erase(column);
          row.strings.erase(column);
        } else if (!string_value.empty() || strcmp(method_name, "nativeSetString") == 0 ||
                   strcmp(method_name, "nativeSetObjectId") == 0 ||
                   strcmp(method_name, "nativeSetUUID") == 0) {
          PFCutRealmStoreStringLocked(state, table, row_id, column, string_value);
        } else {
          PFCutRealmStoreLongLocked(state, table, row_id, column, long_value);
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] table-set method=%s table=%s row=%lld column=%s long=%lld stringLen=%zu\n",
              method_name,
              table.c_str(),
              static_cast<long long>(row_id),
              column.c_str(),
              static_cast<long long>(long_value),
              string_value.size());
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (shorty[0] == 'Z') {
      result->SetZ(false);
      return true;
    }
    if (shorty[0] == 'J') {
      result->SetJ(0);
      return true;
    }
    if (shorty[0] == 'V') {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/OsObject;") == 0) {
    if (strcmp(method_name, "nativeCreateNewObject") == 0 ||
        strcmp(method_name, "nativeCreateRow") == 0 ||
        strstr(method_name, "nativeCreateNewObjectWith") == method_name ||
        strstr(method_name, "nativeCreateRowWith") == method_name) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t row_id = -1;
      std::string table;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        table = PFCutRealmTableForHandleLocked(state, table_handle);
        row_id = PFCutRealmCreateRowTemplateLocked(state, table);
        if (strstr(method_name, "WithLongPrimaryKey") != nullptr) {
          const int64_t column_key = PFCutRealmGetLongParam<is_range>(
              called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, 0);
          const int64_t value = PFCutRealmGetLongParam<is_range>(
              called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 3u, 0);
          PFCutRealmStoreLongLocked(
              state, table, row_id, PFCutRealmColumnNameLocked(state, column_key), value);
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] row-create method=%s table=%s row=%lld\n",
              method_name,
              table.c_str(),
              static_cast<long long>(row_id));
      fflush(stderr);
      result->SetJ(row_id);
      return true;
    }
    if (strcmp(method_name, "nativeCreate") == 0 ||
        strcmp(method_name, "nativeCreateEmbeddedObject") == 0) {
      result->SetJ(PFCutNextRealmPseudoHandle());
      return true;
    }
    if (shorty[0] == 'J') {
      result->SetJ(0);
      return true;
    }
    if (shorty[0] == 'V') {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/objectstore/OsObjectBuilder;") == 0) {
    if (strcmp(method_name, "nativeCreateBuilder") == 0 ||
        strcmp(method_name, "nativeStartList") == 0 ||
        strcmp(method_name, "nativeStartSet") == 0 ||
        strcmp(method_name, "nativeStartDictionary") == 0) {
      const int64_t handle = PFCutNextRealmPseudoHandle();
      {
        std::lock_guard<std::mutex> lock(state.mu);
        if (strcmp(method_name, "nativeCreateBuilder") == 0) {
          state.builders[handle] = {};
        } else {
          state.lists[handle] = {};
        }
      }
      result->SetJ(handle);
      return true;
    }
    if (strstr(method_name, "nativeAdd") == method_name) {
      const int64_t builder_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t column_key = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      std::string column;
      int64_t long_value = 0;
      std::string string_value;
      std::vector<int64_t> list_rows;
      const bool is_string =
          strstr(method_name, "String") != nullptr ||
          strstr(method_name, "ObjectId") != nullptr ||
          strstr(method_name, "UUID") != nullptr;
      const bool is_bool = strstr(method_name, "Boolean") != nullptr;
      const bool is_object_list = strcmp(method_name, "nativeAddObjectList") == 0;
      if (is_string) {
        string_value = PFCutRealmGetStringParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u);
      } else if (is_object_list) {
        list_rows = PFCutRealmGetLongArrayParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u);
      } else if (is_bool) {
        long_value = PFCutRealmGetBoolParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, false) ? 1 : 0;
      } else if (strcmp(method_name, "nativeAddNull") != 0) {
        long_value = PFCutRealmGetLongParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, 0);
      }
      {
        std::lock_guard<std::mutex> lock(state.mu);
        column = PFCutRealmColumnAlias(PFCutRealmColumnNameLocked(state, column_key));
        PFCutRealmBuilderState& builder = state.builders[builder_handle];
        if (is_string) {
          builder.strings[column] = string_value;
        } else if (is_object_list) {
          std::vector<int64_t> row_ids;
          row_ids.reserve(list_rows.size());
          for (int64_t row_value : list_rows) {
            const auto row_it = state.rows.find(row_value);
            row_ids.push_back(row_it != state.rows.end() ? row_it->second.row_id : row_value);
          }
          builder.link_lists[column] = row_ids;
        } else if (strcmp(method_name, "nativeAddNull") != 0) {
          builder.longs[column] = long_value;
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] builder-add method=%s builder=%lld column=%s long=%lld stringLen=%zu listCount=%zu\n",
              method_name,
              static_cast<long long>(builder_handle),
              column.c_str(),
              static_cast<long long>(long_value),
              string_value.size(),
              list_rows.size());
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (strcmp(method_name, "nativeCreateOrUpdateTopLevelObject") == 0 ||
        strcmp(method_name, "nativeUpdateEmbeddedObject") == 0) {
      const int64_t table_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      const int64_t builder_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, 0);
      int64_t row_id = -1;
      std::string table;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        table = PFCutRealmTableForHandleLocked(state, table_handle);
        row_id = PFCutRealmNormalizeTableName(table) == "class_BaseCart" ?
            PFCutRealmEnsureBaseCartRowLocked(state) :
            PFCutRealmCreateRowTemplateLocked(state, table);
        PFCutRealmApplyBuilderLocked(state, table, row_id, builder_handle);
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] builder-commit method=%s table=%s builder=%lld row=%lld\n",
              method_name,
              table.c_str(),
              static_cast<long long>(builder_handle),
              static_cast<long long>(row_id));
      fflush(stderr);
      result->SetJ(row_id);
      return true;
    }
    if (strcmp(method_name, "nativeDestroyBuilder") == 0) {
      const int64_t builder_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.builders.erase(builder_handle);
        state.lists.erase(builder_handle);
      }
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (shorty[0] == 'J') {
      result->SetJ(PFCutNextRealmPseudoHandle());
      return true;
    }
    if (shorty[0] == 'V') {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/OsList;") == 0) {
    if (strcmp(method_name, "nativeCreate") == 0) {
      const int64_t row_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      const int64_t column_key = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, 0);
      const int64_t list_handle = PFCutNextRealmPseudoHandle();
      int64_t target_table_handle = 0;
      std::string table;
      std::string column;
      std::string target_table;
      int64_t size = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto row_it = state.rows.find(row_handle);
        if (row_it != state.rows.end()) {
          table = row_it->second.table;
          column = PFCutRealmColumnAlias(PFCutRealmColumnNameLocked(state, column_key));
          target_table = PFCutRealmLinkListTargetTable(table, column);
          PFCutRealmRowState& owner =
              PFCutRealmEnsureRowTemplateLocked(state, table, row_it->second.row_id);
          PFCutRealmListState list_state;
          list_state.table = table;
          list_state.owner_row_id = row_it->second.row_id;
          list_state.column = column;
          list_state.target_table = target_table;
          const auto persisted_it = owner.link_lists.find(column);
          if (persisted_it != owner.link_lists.end()) {
            list_state.row_ids = persisted_it->second;
          }
          size = static_cast<int64_t>(list_state.row_ids.size());
          state.lists[list_handle] = std::move(list_state);
          if (!target_table.empty()) {
            target_table_handle = PFCutRealmTableHandleLocked(state, target_table);
          }
        } else {
          state.lists[list_handle] = {};
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] list-create list=%lld ownerTable=%s column=%s targetTable=%lld size=%lld\n",
              static_cast<long long>(list_handle),
              table.c_str(),
              column.c_str(),
              static_cast<long long>(target_table_handle),
              static_cast<long long>(size));
      fflush(stderr);
      return PFCutRealmSetLongPairResult(self, list_handle, target_table_handle, result);
    }
    if (strcmp(method_name, "nativeAddRow") == 0 ||
        strcmp(method_name, "nativeInsertRow") == 0) {
      const int64_t list_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const uint32_t row_param = strcmp(method_name, "nativeInsertRow") == 0 ? 2u : 1u;
      const int64_t row_arg = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, row_param, -1);
      int64_t row_id = row_arg;
      std::string table;
      std::string column;
      int64_t size = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        PFCutRealmListState& list_state = state.lists[list_handle];
        const auto row_it = state.rows.find(row_arg);
        if (row_it != state.rows.end()) {
          row_id = row_it->second.row_id;
        }
        if (strcmp(method_name, "nativeInsertRow") == 0) {
          const int64_t index = PFCutRealmGetLongParam<is_range>(
              called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
          const size_t insert_at = index < 0 ? 0 :
              std::min(static_cast<size_t>(index), list_state.row_ids.size());
          list_state.row_ids.insert(list_state.row_ids.begin() + insert_at, row_id);
        } else {
          list_state.row_ids.push_back(row_id);
        }
        if (!list_state.table.empty() && list_state.owner_row_id >= 0 && !list_state.column.empty()) {
          PFCutRealmRowState& owner = PFCutRealmEnsureRowTemplateLocked(
              state, list_state.table, list_state.owner_row_id);
          owner.link_lists[list_state.column] = list_state.row_ids;
        }
        table = list_state.table;
        column = list_state.column;
        size = static_cast<int64_t>(list_state.row_ids.size());
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] list-row method=%s list=%lld ownerTable=%s column=%s row=%lld size=%lld\n",
              method_name,
              static_cast<long long>(list_handle),
              table.c_str(),
              column.c_str(),
              static_cast<long long>(row_id),
              static_cast<long long>(size));
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (strcmp(method_name, "nativeGetRow") == 0) {
      const int64_t list_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t index = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      int64_t row_id = -1;
      int64_t row_handle = 0;
      std::string target_table;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto list_it = state.lists.find(list_handle);
        if (list_it != state.lists.end() &&
            index >= 0 &&
            index < static_cast<int64_t>(list_it->second.row_ids.size())) {
          row_id = list_it->second.row_ids[static_cast<size_t>(index)];
          target_table = list_it->second.target_table;
          if (target_table.empty()) {
            target_table = PFCutRealmLinkListTargetTable(list_it->second.table,
                                                        list_it->second.column);
          }
          if (!target_table.empty() && row_id >= 0) {
            row_handle = PFCutRealmCreateRowHandleLocked(state, target_table, row_id);
          }
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] list-get-row list=%lld index=%lld row=%lld handle=%lld target=%s\n",
              static_cast<long long>(list_handle),
              static_cast<long long>(index),
              static_cast<long long>(row_id),
              static_cast<long long>(row_handle),
              target_table.c_str());
      fflush(stderr);
      result->SetJ(row_handle != 0 ? row_handle : row_id);
      return true;
    }
    if (strstr(method_name, "nativeSize") != nullptr) {
      const int64_t list_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t size = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto list_it = state.lists.find(list_handle);
        if (list_it != state.lists.end()) {
          size = static_cast<int64_t>(list_it->second.row_ids.size());
        }
      }
      result->SetJ(size);
      return true;
    }
    if (shorty[0] == 'J') {
      result->SetJ(0);
      return true;
    }
    if (shorty[0] == 'Z') {
      result->SetZ(false);
      return true;
    }
    if (shorty[0] == 'V') {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/TableQuery;") == 0) {
    if (strcmp(method_name, "nativeRawPredicate") == 0) {
      const int64_t query_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const std::string predicate = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u);
      const std::vector<int64_t> bind_handles = PFCutRealmGetLongArrayParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u);
      std::string table;
      std::string bind0s;
      int64_t bind0l = 0;
      size_t rows = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        auto it = state.queries.find(query_handle);
        if (it != state.queries.end()) {
          it->second.predicate = predicate;
          it->second.bind_handles = bind_handles;
          PFCutRealmDecodeQueryBindsLocked(state, &it->second);
          PFCutRealmMaybePopulateQueryRowsLocked(state, &it->second);
          table = it->second.table;
          if (!it->second.bind_strings.empty()) {
            bind0s = it->second.bind_strings.front();
          }
          if (!it->second.bind_longs.empty()) {
            bind0l = it->second.bind_longs.front();
          }
          rows = it->second.row_ids.size();
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] raw-predicate query=%lld table=%s predicate=%s binds=%zu bind0s=%s bind0l=%lld rows=%zu\n",
              static_cast<long long>(query_handle),
              table.c_str(),
              predicate.c_str(),
              bind_handles.size(),
              bind0s.c_str(),
              static_cast<long long>(bind0l),
              rows);
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (strcmp(method_name, "nativeFind") == 0 ||
        strcmp(method_name, "nativeFindFirst") == 0) {
      const int64_t query_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t row_id = -1;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.queries.find(query_handle);
        if (it != state.queries.end() && !it->second.row_ids.empty()) {
          row_id = it->second.row_ids.front();
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] query-find query=%lld row=%lld\n",
              static_cast<long long>(query_handle),
              static_cast<long long>(row_id));
      fflush(stderr);
      result->SetJ(row_id);
      return true;
    }
    if (strcmp(method_name, "nativeCount") == 0) {
      const int64_t query_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t count = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.queries.find(query_handle);
        if (it != state.queries.end()) {
          count = static_cast<int64_t>(it->second.row_ids.size());
        }
      }
      result->SetJ(count);
      return true;
    }
    if (strcmp(method_name, "nativeRemove") == 0) {
      result->SetJ(0);
      return true;
    }
    if (shorty[0] == 'V') {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/OsResults;") == 0) {
    if (strcmp(method_name, "nativeCreateResults") == 0) {
      const int64_t query_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      const int64_t result_handle = PFCutNextRealmPseudoHandle();
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto query_it = state.queries.find(query_handle);
        if (query_it != state.queries.end()) {
          state.results[result_handle] =
              {query_it->second.table_handle,
               query_handle,
               query_it->second.table,
               query_it->second.row_ids};
        } else {
          state.results[result_handle] = {0, query_handle, "", {}};
        }
      }
      size_t size = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.results.find(result_handle);
        if (it != state.results.end()) {
          size = it->second.row_ids.size();
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] result query=%lld result=%lld size=%zu\n",
              static_cast<long long>(query_handle),
              static_cast<long long>(result_handle),
              size);
      fflush(stderr);
      result->SetJ(result_handle);
      return true;
    }
    if (strcmp(method_name, "nativeSize") == 0) {
      const int64_t result_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t size = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.results.find(result_handle);
        if (it != state.results.end()) {
          size = PFCutRealmResultSizeLocked(it->second);
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] result-size result=%lld size=%lld\n",
              static_cast<long long>(result_handle),
              static_cast<long long>(size));
      fflush(stderr);
      result->SetJ(size);
      return true;
    }
    if (strcmp(method_name, "nativeGetTable") == 0) {
      const int64_t result_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t table_handle = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.results.find(result_handle);
        if (it != state.results.end()) {
          table_handle = it->second.table_handle;
        }
      }
      result->SetJ(table_handle);
      return true;
    }
    if (strcmp(method_name, "nativeWhere") == 0) {
      const int64_t result_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t query_handle = PFCutNextRealmPseudoHandle();
      int64_t table_handle = 0;
      std::string table;
      size_t rows = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.results.find(result_handle);
        if (it != state.results.end()) {
          table_handle = it->second.table_handle;
          table = it->second.table;
          PFCutRealmQueryState query;
          query.table_handle = table_handle;
          query.table = table;
          query.row_ids = it->second.row_ids;
          rows = query.row_ids.size();
          state.queries[query_handle] = query;
        } else {
          state.queries[query_handle] = {0, "", ""};
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] result-where result=%lld table=%s query=%lld rows=%zu\n",
              static_cast<long long>(result_handle),
              table.c_str(),
              static_cast<long long>(query_handle),
              rows);
      fflush(stderr);
      result->SetJ(query_handle);
      return true;
    }
    if (strcmp(method_name, "nativeGetMode") == 0) {
      result->SetB(0);
      return true;
    }
    if (strcmp(method_name, "nativeFirstRow") == 0 ||
        strcmp(method_name, "nativeLastRow") == 0 ||
        strcmp(method_name, "nativeGetRow") == 0) {
      const int64_t result_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t index = 0;
      if (strcmp(method_name, "nativeLastRow") == 0) {
        index = -1;
      } else if (strcmp(method_name, "nativeGetRow") == 0) {
        index = PFCutRealmGetLongParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      }
      int64_t row_handle = 0;
      int64_t row_id = -1;
      std::string table;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.results.find(result_handle);
        if (it != state.results.end() && !it->second.row_ids.empty()) {
          const int64_t resolved_index =
              index < 0 ? static_cast<int64_t>(it->second.row_ids.size()) - 1 : index;
          if (resolved_index >= 0 &&
              resolved_index < static_cast<int64_t>(it->second.row_ids.size())) {
            row_id = it->second.row_ids[static_cast<size_t>(resolved_index)];
            table = it->second.table;
            row_handle = PFCutRealmCreateRowHandleLocked(state, table, row_id);
          }
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] result-row method=%s result=%lld table=%s row=%lld handle=%lld\n",
              method_name,
              static_cast<long long>(result_handle),
              table.c_str(),
              static_cast<long long>(row_id),
              static_cast<long long>(row_handle));
      fflush(stderr);
      result->SetJ(row_handle);
      return true;
    }
    if (strcmp(method_name, "nativeIndexOf") == 0) {
      result->SetJ(-1);
      return true;
    }
    if (shorty[0] == 'Z') {
      result->SetZ(false);
      return true;
    }
    if (shorty[0] == 'J') {
      result->SetJ(0);
      return true;
    }
    if (shorty[0] == 'V') {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  if (strcmp(descriptor, "Lio/realm/internal/UncheckedRow;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/CheckedRow;") == 0) {
    if (strcmp(method_name, "nativeGetColumnKey") == 0) {
      const int64_t row_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const std::string column = PFCutRealmGetStringParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u);
      int64_t column_key = 0;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        column_key =
            PFCutRealmStableColumnKeyLocked(state, PFCutRealmTableForHandleLocked(state, row_handle), column);
      }
      result->SetJ(column_key);
      return true;
    }
    if (strcmp(method_name, "nativeGetColumnNames") == 0) {
      const int64_t row_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      std::vector<std::string> columns;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        columns = PFCutRealmKnownColumnsForTable(PFCutRealmTableForHandleLocked(state, row_handle));
      }
      return PFCutRealmSetStringArrayResult(self, columns, result);
    }
    if (strcmp(method_name, "nativeIsValid") == 0) {
      const int64_t row_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      bool valid = false;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        valid = state.rows.find(row_handle) != state.rows.end();
      }
      result->SetZ(valid);
      return true;
    }
    if (strcmp(method_name, "nativeGetObjectKey") == 0) {
      const int64_t row_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      int64_t row_id = -1;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto it = state.rows.find(row_handle);
        if (it != state.rows.end()) {
          row_id = it->second.row_id;
        }
      }
      result->SetJ(row_id);
      return true;
    }
    if (strcmp(method_name, "nativeGetLong") == 0 ||
        strcmp(method_name, "nativeGetTimestamp") == 0) {
      const int64_t row_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t column_key = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      int64_t value = 0;
      std::string column;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        column = PFCutRealmColumnAlias(PFCutRealmColumnNameLocked(state, column_key));
        const auto row_it = state.rows.find(row_handle);
        if (row_it != state.rows.end()) {
          const auto long_it = row_it->second.longs.find(column);
          if (long_it != row_it->second.longs.end()) {
            value = long_it->second;
          }
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] row-long row=%lld column=%s value=%lld\n",
              static_cast<long long>(row_handle),
              column.c_str(),
              static_cast<long long>(value));
      fflush(stderr);
      result->SetJ(value);
      return true;
    }
    if (strcmp(method_name, "nativeGetString") == 0 ||
        strcmp(method_name, "nativeGetObjectId") == 0 ||
        strcmp(method_name, "nativeGetUUID") == 0) {
      const int64_t row_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t column_key = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      std::string value;
      std::string column;
      {
        std::lock_guard<std::mutex> lock(state.mu);
        column = PFCutRealmColumnAlias(PFCutRealmColumnNameLocked(state, column_key));
        const auto row_it = state.rows.find(row_handle);
        if (row_it != state.rows.end()) {
          const auto str_it = row_it->second.strings.find(column);
          if (str_it != row_it->second.strings.end()) {
            value = str_it->second;
          }
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-STATE] row-string row=%lld column=%s valueLen=%zu\n",
              static_cast<long long>(row_handle),
              column.c_str(),
              value.size());
      fflush(stderr);
      return PFCutRealmSetStringResult(self, value, result);
    }
    if (strstr(method_name, "nativeSet") == method_name ||
        strcmp(method_name, "nativeNullifyLink") == 0) {
      const int64_t row_handle = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 0u, 0);
      const int64_t column_key = PFCutRealmGetLongParam<is_range>(
          called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 1u, 0);
      std::string table;
      std::string column;
      int64_t row_id = -1;
      int64_t long_value = 0;
      std::string string_value;
      const bool is_string =
          strcmp(method_name, "nativeSetString") == 0 ||
          strcmp(method_name, "nativeSetObjectId") == 0 ||
          strcmp(method_name, "nativeSetUUID") == 0;
      if (is_string) {
        string_value = PFCutRealmGetStringParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u);
      } else if (strcmp(method_name, "nativeSetBoolean") == 0) {
        long_value = PFCutRealmGetBoolParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, false) ? 1 : 0;
      } else if (strcmp(method_name, "nativeSetNull") != 0 &&
                 strcmp(method_name, "nativeNullifyLink") != 0) {
        long_value = PFCutRealmGetLongParam<is_range>(
            called_method, shorty, shadow_frame, number_of_inputs, arg, vregC, 2u, 0);
      }
      {
        std::lock_guard<std::mutex> lock(state.mu);
        const auto row_it = state.rows.find(row_handle);
        if (row_it != state.rows.end()) {
          table = row_it->second.table;
          row_id = row_it->second.row_id;
        }
        column = PFCutRealmColumnAlias(PFCutRealmColumnNameLocked(state, column_key));
        if (strcmp(method_name, "nativeSetNull") == 0 ||
            strcmp(method_name, "nativeNullifyLink") == 0) {
          PFCutRealmRowState& row = PFCutRealmEnsureRowTemplateLocked(state, table, row_id);
          row.longs.erase(column);
          row.strings.erase(column);
        } else if (is_string) {
          PFCutRealmStoreStringLocked(state, table, row_id, column, string_value);
        } else {
          PFCutRealmStoreLongLocked(state, table, row_id, column, long_value);
        }
      }
      fprintf(stderr,
              "[PFCUT-REALM-WRITE] row-set method=%s table=%s row=%lld column=%s long=%lld stringLen=%zu\n",
              method_name,
              table.c_str(),
              static_cast<long long>(row_id),
              column.c_str(),
              static_cast<long long>(long_value),
              string_value.size());
      fflush(stderr);
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    if (shorty[0] == 'Z') {
      result->SetZ(false);
      return true;
    }
    if (shorty[0] == 'I') {
      result->SetI(0);
      return true;
    }
    if (shorty[0] == 'J') {
      result->SetJ(0);
      return true;
    }
    if (shorty[0] == 'V') {
      PFCutSetDefaultResultForShorty(shorty, result);
      return true;
    }
    return false;
  }

  return false;
}

static inline bool PFCutRealmNativeLongLooksLikeHandle(const char* method_name) {
  if (method_name == nullptr) {
    return false;
  }
  if (strcmp(method_name, "nativeGetFinalizerPtr") == 0 ||
      strcmp(method_name, "nativeGetFinalizerMethodPtr") == 0 ||
      strcmp(method_name, "nativeGetSchemaVersion") == 0 ||
      strstr(method_name, "nativeSize") != nullptr ||
      strstr(method_name, "nativeCount") != nullptr ||
      strstr(method_name, "nativeFindFirst") != nullptr ||
      strstr(method_name, "nativeGetColumnCount") != nullptr ||
      strstr(method_name, "nativeGetColumnKey") != nullptr ||
      strstr(method_name, "nativeGetColumnType") != nullptr ||
      strstr(method_name, "nativeGetObjectKey") != nullptr ||
      strstr(method_name, "nativeGetLong") != nullptr ||
      strstr(method_name, "nativeGetTimestamp") != nullptr) {
    return false;
  }
  return strstr(method_name, "nativeCreate") != nullptr ||
      strstr(method_name, "nativeGetSharedRealm") != nullptr ||
      strstr(method_name, "nativeGetSchemaInfo") != nullptr ||
      strstr(method_name, "nativeGetTableRef") != nullptr ||
      strstr(method_name, "nativeGetObjectSchemaInfo") != nullptr ||
      strstr(method_name, "nativeWhere") != nullptr ||
      strstr(method_name, "nativeFreeze") != nullptr ||
      strstr(method_name, "nativeStart") != nullptr ||
      strstr(method_name, "nativeGetRowPtr") != nullptr ||
      strstr(method_name, "nativeGetLink") != nullptr ||
      strstr(method_name, "nativeCreateMapping") != nullptr;
}

static inline bool PFCutRealmNativeLooksLikeDataAccess(const char* method_name) {
  if (method_name == nullptr) {
    return false;
  }
  return strstr(method_name, "nativeSize") != nullptr ||
      strstr(method_name, "nativeFindFirst") != nullptr ||
      strstr(method_name, "nativeFirstRow") != nullptr ||
      strstr(method_name, "nativeLastRow") != nullptr ||
      strstr(method_name, "nativeGetRow") != nullptr ||
      strstr(method_name, "nativeGetValue") != nullptr ||
      strstr(method_name, "nativeGetString") != nullptr ||
      strstr(method_name, "nativeGetLong") != nullptr ||
      strstr(method_name, "nativeGetBoolean") != nullptr ||
      strstr(method_name, "nativeIndexOf") != nullptr ||
      strstr(method_name, "nativeSet") != nullptr ||
      strstr(method_name, "nativeAdd") != nullptr ||
      strstr(method_name, "nativeCreateOrUpdate") != nullptr ||
      strstr(method_name, "nativeCommitTransaction") != nullptr ||
      strstr(method_name, "nativeBeginTransaction") != nullptr ||
      strstr(method_name, "nativeCancelTransaction") != nullptr;
}

static inline bool PFCutTrySetRealmNativeBoundaryResult(ArtMethod* called_method,
                                                        const char* shorty,
                                                        JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || shorty == nullptr || result == nullptr) {
    return false;
  }
  const char* method_name = WlSafeName(called_method);
  if (method_name == nullptr) {
    return false;
  }
  if (shorty[0] == 'J') {
    int64_t value = 0;
    if (strstr(method_name, "nativeFindFirst") != nullptr) {
      value = -1;
    } else if (PFCutRealmNativeLongLooksLikeHandle(method_name)) {
      value = PFCutNextRealmPseudoHandle();
    }
    result->SetJ(value);
    if (value != 0) {
      static thread_local int realm_handle_count = 0;
      if (realm_handle_count < 80 || WestlakeTraceVerboseCalls()) {
        realm_handle_count++;
        fprintf(stderr,
                "[PFCUT-REALM] native boundary pseudo-handle %lld %s shorty=%s\n",
                static_cast<long long>(value),
                called_method->PrettyMethod().c_str(),
                shorty);
        fflush(stderr);
      }
    }
    return true;
  }
  if (shorty[0] == 'Z') {
    bool value = false;
    if (strstr(method_name, "nativeIsValid") != nullptr ||
        strstr(method_name, "nativeHasColumn") != nullptr ||
        strstr(method_name, "nativeCallWithLock") != nullptr) {
      value = true;
    }
    result->SetZ(value);
    return true;
  }
  if (shorty[0] == 'L' &&
      (strstr(method_name, "nativeGetName") != nullptr ||
       strstr(method_name, "nativeGetString") != nullptr)) {
    Thread* self = Thread::Current();
    ObjPtr<mirror::String> value =
        mirror::String::AllocFromModifiedUtf8(self, "");
    if (self->IsExceptionPending()) {
      self->ClearException();
      return false;
    }
    result->SetL(value);
    return true;
  }
  return false;
}

template <bool is_range>
static inline void PFCutLogRealmNativeBoundaryArgs(
    ArtMethod* called_method,
    const char* descriptor,
    const char* shorty,
    ShadowFrame& shadow_frame,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || descriptor == nullptr || shorty == nullptr) {
    return;
  }
  const char* method_name = WlSafeName(called_method);
  const bool interesting =
      PFCutRealmNativeLooksLikeDataAccess(method_name) ||
      strcmp(descriptor, "Lio/realm/internal/OsSharedRealm;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/NativeObjectReference;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/Table;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/TableQuery;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/OsResults;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/OsObject;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/OsList;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/objectstore/OsObjectBuilder;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/UncheckedRow;") == 0 ||
      strcmp(descriptor, "Lio/realm/internal/CheckedRow;") == 0;
  if (!interesting) {
    return;
  }

  static thread_local int realm_arg_log_count = 0;
  if (realm_arg_log_count >= 260 && !WestlakeTraceVerboseCalls()) {
    return;
  }
  realm_arg_log_count++;

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };
  ArtMethod* caller = shadow_frame.GetMethod();
  fprintf(stderr,
          "[PFCUT-REALM-ARGS] callee=%s caller=%s inputs=%u static=%d args=",
          called_method->PrettyMethod().c_str(),
          caller != nullptr ? caller->PrettyMethod().c_str() : "<null>",
          number_of_inputs,
          called_method->IsStatic() ? 1 : 0);

  uint32_t slot = 0;
  if (!called_method->IsStatic() && slot < number_of_inputs) {
    fprintf(stderr, " receiver(r%u)=", reg_at(slot));
    PFCutPrintObjectSummary(shadow_frame.GetVRegReference(reg_at(slot)), 0);
    slot++;
  }

  for (uint32_t shorty_index = 1; shorty[shorty_index] != '\0' && slot < number_of_inputs;
       ++shorty_index) {
    const char kind = shorty[shorty_index];
    const uint32_t reg = reg_at(slot);
    switch (kind) {
      case 'J':
        fprintf(stderr,
                " J%u(r%u)=%lld",
                shorty_index - 1,
                reg,
                static_cast<long long>(shadow_frame.GetVRegLong(reg)));
        slot += 2;
        break;
      case 'D':
        fprintf(stderr,
                " D%u(r%u)=0x%llx",
                shorty_index - 1,
                reg,
                static_cast<unsigned long long>(shadow_frame.GetVRegLong(reg)));
        slot += 2;
        break;
      case 'F':
      case 'I':
      case 'B':
      case 'C':
      case 'S':
      case 'Z':
        fprintf(stderr,
                " %c%u(r%u)=%d",
                kind,
                shorty_index - 1,
                reg,
                shadow_frame.GetVReg(reg));
        slot++;
        break;
      case 'L':
      case '[':
        fprintf(stderr, " %c%u(r%u)=", kind, shorty_index - 1, reg);
        PFCutPrintObjectSummary(shadow_frame.GetVRegReference(reg), 0);
        slot++;
        break;
      default:
        fprintf(stderr, " %c%u(r%u)=?", kind, shorty_index - 1, reg);
        slot++;
        break;
    }
  }
  fprintf(stderr, "\n");
  fflush(stderr);
}

template <bool is_range>
static inline bool PFCutTryRealmNativeBoundaryNoop(
    ArtMethod* called_method,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsNative() ||
      called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  std::string descriptor_storage;
  const char* descriptor = called_method->GetDeclaringClass()->GetDescriptor(&descriptor_storage);
  static constexpr const char* kRealmPrefix = "Lio/realm/";
  if (descriptor == nullptr ||
      strncmp(descriptor, kRealmPrefix, strlen(kRealmPrefix)) != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || shorty_len == 0u) {
    return false;
  }
  PFCutLogRealmNativeBoundaryArgs<is_range>(
      called_method, descriptor, shorty, shadow_frame, number_of_inputs, arg, vregC);

  if (strcmp(descriptor, "Lio/realm/internal/Util;") == 0 &&
      strcmp(WlSafeName(called_method), "nativeGetTablePrefix") == 0) {
    Thread* self = Thread::Current();
    ObjPtr<mirror::String> prefix = mirror::String::AllocFromModifiedUtf8(self, "class_");
    if (self->IsExceptionPending()) {
      self->ClearException();
      return false;
    }
    result->SetL(prefix);
    static thread_local int realm_table_prefix_count = 0;
    if (realm_table_prefix_count < 12 || WestlakeTraceVerboseCalls()) {
      realm_table_prefix_count++;
      fprintf(stderr,
              "[PFCUT-REALM] native table prefix %s shorty=%s quick=%p jni=%p\n",
              called_method->PrettyMethod().c_str(),
              shorty,
              called_method->GetEntryPointFromQuickCompiledCode(),
              called_method->GetEntryPointFromJni());
      fflush(stderr);
    }
    return true;
  }

  if (strcmp(descriptor, "Lio/realm/internal/TableQuery;") == 0 &&
      strcmp(WlSafeName(called_method), "nativeValidateQuery") == 0) {
    Thread* self = Thread::Current();
    gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
    ObjPtr<mirror::Object> empty = mirror::String::AllocEmptyString(self, allocator_type);
    result->SetL(empty);
    static thread_local int realm_validate_noop_count = 0;
    if (realm_validate_noop_count < 12 || WestlakeTraceVerboseCalls()) {
      realm_validate_noop_count++;
      fprintf(stderr,
              "[PFCUT-REALM] native boundary validate-ok %s shorty=%s quick=%p jni=%p\n",
              called_method->PrettyMethod().c_str(),
              shorty,
              called_method->GetEntryPointFromQuickCompiledCode(),
              called_method->GetEntryPointFromJni());
      fflush(stderr);
    }
    return true;
  }

  if (PFCutTryRealmNativeState<is_range>(
          called_method,
          descriptor,
          shorty,
          shadow_frame,
          result,
          number_of_inputs,
          arg,
          vregC)) {
    return true;
  }

  static thread_local int realm_native_noop_count = 0;
  static thread_local int realm_native_data_probe_count = 0;
  const bool data_probe = PFCutRealmNativeLooksLikeDataAccess(WlSafeName(called_method)) &&
      realm_native_data_probe_count < 160;
  if (data_probe) {
    realm_native_data_probe_count++;
  }
  if (realm_native_noop_count < 24 || data_probe || WestlakeTraceVerboseCalls()) {
    realm_native_noop_count++;
    fprintf(stderr,
            "[PFCUT-REALM] native boundary noop %s shorty=%s quick=%p jni=%p\n",
            called_method->PrettyMethod().c_str(),
            shorty,
            called_method->GetEntryPointFromQuickCompiledCode(),
            called_method->GetEntryPointFromJni());
    fflush(stderr);
  }

  // Boundary probe only. Realm persistence is not implemented here; this
  // preserves ABI-shaped return values so the next app/runtime blocker is
  // visible while PF-494 tracks real portable APK native-library loading.
  if (!PFCutTrySetRealmNativeBoundaryResult(called_method, shorty, result)) {
    PFCutSetDefaultResultForShorty(shorty, result);
  }
  return true;
}

template <bool is_range>
static inline bool PFCutTryAndroidxWorkManagerConstructorLite(
    ArtMethod* called_method,
    ShadowFrame& shadow_frame,
    JValue* result,
    uint16_t number_of_inputs,
    uint32_t (&arg)[Instruction::kMaxVarArgRegs],
    uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 8u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals(
          "Landroidx/work/impl/WorkManagerImpl;") ||
      strcmp(WlSafeName(called_method), "<init>") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "VLLLLLLL") != 0) {
    return false;
  }

  const auto reg_at = [&](uint32_t input_index) -> uint32_t {
    return is_range ? (vregC + input_index) : arg[input_index];
  };
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(reg_at(0));
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    if (result != nullptr) {
      result->SetJ(0);
    }
    return true;
  }

  ObjPtr<mirror::Class> klass = called_method->GetDeclaringClass();
  const auto set_object_field = [&](const char* name,
                                    const char* descriptor,
                                    uint32_t input_index) {
    ArtField* field = klass->FindInstanceField(name, descriptor);
    if (field != nullptr) {
      field->SetObject<false>(receiver, shadow_frame.GetVRegReference(reg_at(input_index)));
    }
  };

  set_object_field("b", "Landroid/content/Context;", 1u);
  set_object_field("c", "Landroidx/work/Configuration;", 2u);
  set_object_field("e", "Landroidx/work/impl/utils/taskexecutor/TaskExecutor;", 3u);
  set_object_field("d", "Landroidx/work/impl/WorkDatabase;", 4u);
  set_object_field("f", "Ljava/util/List;", 5u);
  set_object_field("g", "Landroidx/work/impl/Processor;", 6u);
  set_object_field("k", "Landroidx/work/impl/constraints/trackers/Trackers;", 7u);

  static thread_local int workmanager_lite_count = 0;
  if (workmanager_lite_count < 24 || WestlakeTraceVerboseCalls()) {
    workmanager_lite_count++;
    fprintf(stderr,
            "[PFCUT-WORK] WorkManagerImpl constructor-lite fields seeded %s shorty=%s\n",
            called_method->PrettyMethod().c_str(),
            shorty);
    fflush(stderr);
  }

  if (result != nullptr) {
    result->SetJ(0);
  }
  return true;
}

static inline bool PFCutTryAndroidxSplashNoop(ArtMethod* called_method, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  std::string descriptor_storage;
  const char* descriptor = called_method->GetDeclaringClass()->GetDescriptor(&descriptor_storage);
  if (descriptor == nullptr || strstr(descriptor, "Landroidx/core/splashscreen/") == nullptr) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || shorty_len == 0u) {
    return false;
  }

  static thread_local int splash_noop_count = 0;
  if (splash_noop_count < 40) {
    splash_noop_count++;
    fprintf(stderr,
            "[PFCUT] AndroidX splash noop %s shorty=%s quick=%p jni=%p native=%d code=%p\n",
            called_method->PrettyMethod().c_str(),
            shorty,
            called_method->GetEntryPointFromQuickCompiledCode(),
            called_method->IsNative() ? called_method->GetEntryPointFromJni() : nullptr,
            called_method->IsNative() ? 1 : 0,
            called_method->GetCodeItem());
    fflush(stderr);
  }

  result->SetJ(0);
  return true;
}

static inline bool PFCutTryAndroidxLifecycleGeneratedAdapterFallback(ArtMethod* called_method,
                                                                     JValue* result,
                                                                     uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      called_method->IsStatic() ||
      number_of_inputs != 2u ||
      called_method->GetDeclaringClass() == nullptr ||
      !called_method->GetDeclaringClass()->DescriptorEquals("Landroidx/lifecycle/Lifecycling;") ||
      strcmp(WlSafeName(called_method), "b") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "LL") != 0) {
    return false;
  }

  static thread_local int lifecycle_generated_adapter_fallback_count = 0;
  if (lifecycle_generated_adapter_fallback_count < 80) {
    lifecycle_generated_adapter_fallback_count++;
    fprintf(stderr,
            "[PFCUT] AndroidX Lifecycling generated-adapter fallback %s shorty=%s\n",
            called_method->PrettyMethod().c_str(),
            shorty);
    fflush(stderr);
  }

  // Equivalent to Lifecycling.b(Class) catching ClassNotFoundException for a
  // generated *_LifecycleAdapter probe and returning null.  This keeps AndroidX
  // on its documented reflective-observer path without exercising stale native
  // exception/allocation state in standalone Westlake.
  result->SetL(nullptr);
  return true;
}

template <bool is_range>
static inline bool PFCutTryNewRelicNoop(ArtMethod* called_method,
                                        ShadowFrame& shadow_frame,
                                        JValue* result,
                                        uint16_t number_of_inputs,
                                        uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                        uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  std::string descriptor_storage;
  const char* descriptor = called_method->GetDeclaringClass()->GetDescriptor(&descriptor_storage);
  if (descriptor == nullptr || strstr(descriptor, "Lcom/newrelic/agent/android/") == nullptr) {
    return false;
  }
  if (strcmp(descriptor, "Lcom/newrelic/agent/android/instrumentation/GsonInstrumentation;") == 0) {
    return false;
  }
  if (strcmp(descriptor,
             "Lcom/newrelic/agent/android/instrumentation/BitmapFactoryInstrumentation;") == 0) {
    return false;
  }
  if (strcmp(descriptor, "Lcom/newrelic/agent/android/util/Util;") == 0) {
    const char* method_name = WlSafeName(called_method);
    if (method_name != nullptr && strcmp(method_name, "getRandom") == 0) {
      return false;
    }
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || shorty_len == 0u) {
    return false;
  }

  const char* method_name = WlSafeName(called_method);
  if (strcmp(descriptor,
             "Lcom/newrelic/agent/android/background/ApplicationStateMonitor;") == 0 &&
      called_method->IsStatic() &&
      method_name != nullptr &&
      strcmp(method_name, "getInstance") == 0 &&
      shorty[0] == 'L') {
    Thread* self = Thread::Current();
    ObjPtr<mirror::Object> monitor =
        called_method->GetDeclaringClass()->AllocObject(self);
    static thread_local int newrelic_monitor_count = 0;
    if (newrelic_monitor_count < 20 || WestlakeTraceVerboseCalls()) {
      newrelic_monitor_count++;
      fprintf(stderr,
              "[PFCUT] NewRelic ApplicationStateMonitor singleton allocated %s object=%p\n",
              called_method->PrettyMethod().c_str(),
              monitor.Ptr());
      fflush(stderr);
    }
    result->SetL(monitor);
    return true;
  }

  if (strcmp(descriptor,
             "Lcom/newrelic/agent/android/instrumentation/URLConnectionInstrumentation;") == 0 &&
      called_method->IsStatic() &&
      method_name != nullptr &&
      (strcmp(method_name, "openConnection") == 0 ||
       strcmp(method_name, "openConnectionWithProxy") == 0) &&
      strcmp(shorty, "LL") == 0 &&
      number_of_inputs >= 1u) {
    const auto reg_at = [&](uint32_t input_index) -> uint32_t {
      return is_range ? (vregC + input_index) : arg[input_index];
    };
    ObjPtr<mirror::Object> connection = shadow_frame.GetVRegReference(reg_at(0));
    static thread_local int newrelic_url_passthrough_count = 0;
    if (newrelic_url_passthrough_count < 80 || WestlakeTraceVerboseCalls()) {
      newrelic_url_passthrough_count++;
      fprintf(stderr,
              "[PFCUT] NewRelic URLConnection passthrough %s connection=%p\n",
              called_method->PrettyMethod().c_str(),
              connection.Ptr());
      fflush(stderr);
    }
    result->SetL(connection);
    return true;
  }

  static thread_local int newrelic_noop_count = 0;
  if (newrelic_noop_count < 80) {
    newrelic_noop_count++;
    fprintf(stderr,
            "[PFCUT] NewRelic noop %s shorty=%s quick=%p jni=%p native=%d code=%p\n",
            called_method->PrettyMethod().c_str(),
            shorty,
            called_method->GetEntryPointFromQuickCompiledCode(),
            called_method->IsNative() ? called_method->GetEntryPointFromJni() : nullptr,
            called_method->IsNative() ? 1 : 0,
            called_method->GetCodeItem());
    fflush(stderr);
  }

  // New Relic is telemetry instrumentation. It is safe to make it inert while
  // proving the real app UI/runtime path; returning zero/null matches disabled
  // agent behavior for the primitive and reference return types used here.
  result->SetJ(0);
  return true;
}

static inline bool PFCutTryKotlinCoroutineExceptionNoop(ArtMethod* called_method,
                                                        JValue* result,
                                                        uint16_t number_of_inputs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr ||
      !called_method->IsStatic() ||
      number_of_inputs != 2u ||
      called_method->GetDeclaringClass() == nullptr) {
    return false;
  }

  std::string descriptor_storage;
  const char* descriptor = called_method->GetDeclaringClass()->GetDescriptor(&descriptor_storage);
  if (descriptor == nullptr ||
      (strcmp(descriptor,
              "Lkotlinx/coroutines/internal/CoroutineExceptionHandlerImpl_commonKt;") != 0 &&
       strcmp(descriptor, "Lkotlinx/coroutines/CoroutineExceptionHandlerKt;") != 0)) {
    return false;
  }

  const char* method_name = WlSafeName(called_method);
  if (method_name == nullptr || strcmp(method_name, "a") != 0) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty =
      called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);
  if (shorty == nullptr || strcmp(shorty, "VLL") != 0) {
    return false;
  }

  static thread_local int coroutine_exception_noop_count = 0;
  if (coroutine_exception_noop_count < 80) {
    coroutine_exception_noop_count++;
    fprintf(stderr,
            "[PFCUT] Kotlin coroutine exception dispatcher noop %s shorty=%s\n",
            called_method->PrettyMethod().c_str(),
            shorty);
    fflush(stderr);
  }

  // This is only the coroutine global exception reporting path. Current McD
  // proof runs die here because ServiceLoader/classpath fallbacks can leave the
  // handler collection null, then the reporter crashes before startup can expose
  // the next app/runtime boundary.
  result->SetJ(0);
  return true;
}

template <typename T>
bool SendMethodExitEvents(Thread* self,
                          const instrumentation::Instrumentation* instrumentation,
                          ShadowFrame& frame,
                          ArtMethod* method,
                          T& result) {
  bool had_event = false;
  // We can get additional ForcePopFrame requests during handling of these events. We should
  // respect these and send additional instrumentation events.
  do {
    frame.SetForcePopFrame(false);
    if (UNLIKELY(instrumentation->HasMethodExitListeners() && !frame.GetSkipMethodExitEvents())) {
      had_event = true;
      instrumentation->MethodExitEvent(self, method, instrumentation::OptionalFrame{frame}, result);
    }
    // We don't send method-exit if it's a pop-frame. We still send frame_popped though.
    if (UNLIKELY(frame.NeedsNotifyPop() && instrumentation->HasWatchedFramePopListeners())) {
      had_event = true;
      instrumentation->WatchedFramePopped(self, frame);
    }
  } while (UNLIKELY(frame.GetForcePopFrame()));
  if (UNLIKELY(had_event)) {
    return !self->IsExceptionPending();
  } else {
    return true;
  }
}

template
bool SendMethodExitEvents(Thread* self,
                          const instrumentation::Instrumentation* instrumentation,
                          ShadowFrame& frame,
                          ArtMethod* method,
                          MutableHandle<mirror::Object>& result);

template
bool SendMethodExitEvents(Thread* self,
                          const instrumentation::Instrumentation* instrumentation,
                          ShadowFrame& frame,
                          ArtMethod* method,
                          JValue& result);

// We execute any instrumentation events that are triggered by this exception and change the
// shadow_frame's dex_pc to that of the exception handler if there is one in the current method.
// Return true if we should continue executing in the current method and false if we need to go up
// the stack to find an exception handler.
// We accept a null Instrumentation* meaning we must not report anything to the instrumentation.
// TODO We should have a better way to skip instrumentation reporting or possibly rethink that
// behavior.
bool MoveToExceptionHandler(Thread* self,
                            ShadowFrame& shadow_frame,
                            bool skip_listeners,
                            bool skip_throw_listener) {
  self->VerifyStack();
  StackHandleScope<2> hs(self);
  Handle<mirror::Throwable> exception(hs.NewHandle(self->GetException()));
  const instrumentation::Instrumentation* instrumentation =
      Runtime::Current()->GetInstrumentation();
  if (!skip_throw_listener &&
      instrumentation->HasExceptionThrownListeners() &&
      self->IsExceptionThrownByCurrentMethod(exception.Get())) {
    // See b/65049545 for why we don't need to check to see if the exception has changed.
    instrumentation->ExceptionThrownEvent(self, exception.Get());
    if (shadow_frame.GetForcePopFrame()) {
      // We will check in the caller for GetForcePopFrame again. We need to bail out early to
      // prevent an ExceptionHandledEvent from also being sent before popping.
      return true;
    }
  }
  bool clear_exception = false;
  uint32_t found_dex_pc = shadow_frame.GetMethod()->FindCatchBlock(
      hs.NewHandle(exception->GetClass()), shadow_frame.GetDexPC(), &clear_exception);
  if (exception.Get() != nullptr &&
      exception->GetClass() != nullptr &&
      exception->GetClass()->DescriptorEquals("Ljava/lang/IllegalArgumentException;")) {
    std::string message_storage;
    const char* message = PFCutThrowableMessage(exception.Get(), &message_storage);
    if (message != nullptr && strstr(message, "Failed requirement") != nullptr) {
      static thread_local int failed_requirement_unwind_count = 0;
      if (failed_requirement_unwind_count < 120) {
        failed_requirement_unwind_count++;
        ArtMethod* method = shadow_frame.GetMethod();
        fprintf(stderr,
                "[PFCUT-IAE-REQUIRE] method=%s dex_pc=%u catch=%u clear=%d message=%s\n",
                method != nullptr ? method->PrettyMethod().c_str() : "<null>",
                shadow_frame.GetDexPC(),
                found_dex_pc,
                clear_exception ? 1 : 0,
                message);
        fflush(stderr);
      }
    }
  }
  if (found_dex_pc == dex::kDexNoIndex) {
    if (!skip_listeners) {
      if (shadow_frame.NeedsNotifyPop()) {
        instrumentation->WatchedFramePopped(self, shadow_frame);
        if (shadow_frame.GetForcePopFrame()) {
          // We will check in the caller for GetForcePopFrame again. We need to bail out early to
          // prevent an ExceptionHandledEvent from also being sent before popping and to ensure we
          // handle other types of non-standard-exits.
          return true;
        }
      }
      // Exception is not caught by the current method. We will unwind to the
      // caller. Notify any instrumentation listener.
      instrumentation->MethodUnwindEvent(self,
                                         shadow_frame.GetMethod(),
                                         shadow_frame.GetDexPC());
    }
    return shadow_frame.GetForcePopFrame();
  } else {
    shadow_frame.SetDexPC(found_dex_pc);
    if (!skip_listeners && instrumentation->HasExceptionHandledListeners()) {
      self->ClearException();
      instrumentation->ExceptionHandledEvent(self, exception.Get());
      if (UNLIKELY(self->IsExceptionPending())) {
        // Exception handled event threw an exception. Try to find the handler for this one.
        return MoveToExceptionHandler(self, shadow_frame, skip_listeners, skip_throw_listener);
      } else if (!clear_exception) {
        self->SetException(exception.Get());
      }
    } else if (clear_exception) {
      self->ClearException();
    }
    return true;
  }
}

void UnexpectedOpcode(const Instruction* inst, const ShadowFrame& shadow_frame) {
  LOG(FATAL) << "Unexpected instruction: "
             << inst->DumpString(shadow_frame.GetMethod()->GetDexFile());
  UNREACHABLE();
}

// START DECLARATIONS :
//
// These additional declarations are required because clang complains
// about ALWAYS_INLINE (-Werror, -Wgcc-compat) in definitions.
//

template <bool is_range>
NO_STACK_PROTECTOR
static ALWAYS_INLINE bool DoCallCommon(ArtMethod* called_method,
                                       Thread* self,
                                       ShadowFrame& shadow_frame,
                                       JValue* result,
                                       uint16_t number_of_inputs,
                                       uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                       uint32_t vregC,
                                       bool string_init) REQUIRES_SHARED(Locks::mutator_lock_);

template <bool is_range>
ALWAYS_INLINE void CopyRegisters(ShadowFrame& caller_frame,
                                 ShadowFrame* callee_frame,
                                 const uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                 const size_t first_src_reg,
                                 const size_t first_dest_reg,
                                 const size_t num_regs) REQUIRES_SHARED(Locks::mutator_lock_);

// END DECLARATIONS.

static inline bool PFCutLookupProbeInvokeFlag(ArtMethod* caller, ArtMethod* called)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UNUSED(caller);
  UNUSED(called);
  return false;
}

static inline bool PFCutIsL3OnCreate(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UNUSED(method);
  return false;
}

template <bool is_range>
static inline bool PFCutTryProxyInvoke(ArtMethod* called_method,
                                       Thread* self,
                                       ShadowFrame& shadow_frame,
                                       JValue* result,
                                       uint16_t number_of_inputs,
                                       const uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                       uint32_t vregC)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (called_method == nullptr || !called_method->IsProxyMethod()) {
    return false;
  }

  ArtMethod* interface_method = called_method->FindOverriddenMethod(kRuntimePointerSize);
  if (interface_method == nullptr || interface_method->IsProxyMethod()) {
    interface_method = called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
  }
  if (interface_method == nullptr) {
    return false;
  }

  uint32_t shorty_len = 0;
  const char* shorty = interface_method->GetShorty(&shorty_len);
  if (shorty == nullptr || shorty_len == 0u || number_of_inputs == 0u) {
    return false;
  }

  auto input_reg = [&](uint32_t input_index) -> uint32_t {
    return is_range ? vregC + input_index : arg[input_index];
  };

  const uint32_t receiver_reg = input_reg(0);
  ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
  if (receiver == nullptr) {
    ThrowNullPointerExceptionFromInterpreter();
    if (result != nullptr) {
      result->SetJ(0);
    }
    return true;
  }

  ScopedObjectAccessUnchecked soa(self);
  ScopedJniEnvLocalRefState env_state(soa.Env());
  jobject receiver_jobj = soa.AddLocalReference<jobject>(receiver);
  ObjPtr<mirror::Method> interface_reflect_method =
      mirror::Method::CreateFromArtMethod<kRuntimePointerSize>(self, interface_method);
  if (interface_reflect_method == nullptr) {
    self->AssertPendingOOMException();
    return true;
  }
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(interface_reflect_method);

  std::vector<jvalue> proxy_args;
  proxy_args.reserve(shorty_len > 0u ? shorty_len - 1u : 0u);
  uint32_t input_index = 1u;
  for (uint32_t shorty_index = 1u; shorty_index < shorty_len; ++shorty_index) {
    if (input_index >= number_of_inputs) {
      return false;
    }
    const uint32_t reg = input_reg(input_index);
    jvalue value = {};
    switch (shorty[shorty_index]) {
      case 'L':
        value.l = soa.AddLocalReference<jobject>(shadow_frame.GetVRegReference(reg));
        input_index += 1u;
        break;
      case 'Z':
        value.z = static_cast<jboolean>(shadow_frame.GetVReg(reg));
        input_index += 1u;
        break;
      case 'B':
        value.b = static_cast<jbyte>(shadow_frame.GetVReg(reg));
        input_index += 1u;
        break;
      case 'C':
        value.c = static_cast<jchar>(shadow_frame.GetVReg(reg));
        input_index += 1u;
        break;
      case 'S':
        value.s = static_cast<jshort>(shadow_frame.GetVReg(reg));
        input_index += 1u;
        break;
      case 'I':
        value.i = static_cast<jint>(shadow_frame.GetVReg(reg));
        input_index += 1u;
        break;
      case 'F':
        value.f = shadow_frame.GetVRegFloat(reg);
        input_index += 1u;
        break;
      case 'J':
        value.j = static_cast<jlong>(shadow_frame.GetVRegLong(reg));
        input_index += 2u;
        break;
      case 'D':
        value.d = shadow_frame.GetVRegDouble(reg);
        input_index += 2u;
        break;
      default:
        return false;
    }
    proxy_args.push_back(value);
  }

  static thread_local int proxy_invoke_count = 0;
  if (proxy_invoke_count < 80) {
    proxy_invoke_count++;
    fprintf(stderr,
            "[PFCUT-PROXY] interpreter proxy invoke %s via %s args=%zu quick=%p\n",
            called_method->PrettyMethod().c_str(),
            interface_method->PrettyMethod().c_str(),
            proxy_args.size(),
            called_method->GetEntryPointFromQuickCompiledCode());
    fflush(stderr);
  }

  JValue proxy_result = InvokeProxyInvocationHandler(
      soa, shorty, receiver_jobj, interface_method_jobj, proxy_args);
  if (result != nullptr) {
    *result = proxy_result;
  }
  return true;
}

NO_STACK_PROTECTOR
void ArtInterpreterToCompiledCodeBridge(Thread* self,
                                        ArtMethod* caller,
                                        ShadowFrame* shadow_frame,
                                        uint16_t arg_offset,
                                        JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* method = shadow_frame->GetMethod();
  // Basic checks for the arg_offset. If there's no code item, the arg_offset must be 0. Otherwise,
  // check that the arg_offset isn't greater than the number of registers. A stronger check is
  // difficult since the frame may contain space for all the registers in the method, or only enough
  // space for the arguments.
  if (kIsDebugBuild) {
    if (method->GetCodeItem() == nullptr) {
      DCHECK_EQ(0u, arg_offset) << method->PrettyMethod();
    } else {
      DCHECK_LE(arg_offset, shadow_frame->NumberOfVRegs());
    }
  }
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr && caller != nullptr) {
    jit->NotifyInterpreterToCompiledCodeTransition(self, caller);
  }
  // WESTLAKE §601: do not force-interpret a method that HAS live JIT code -- that is the whole
  // point of this bridge. Force-interpreting it re-entered Execute, which re-dispatched here,
  // giving an infinite recursion that died as a StackOverflowError.
  const void* wl_ep = method->GetEntryPointFromQuickCompiledCode();
  const bool wl_has_jit_code =
      (jit != nullptr && jit->GetCodeCache() != nullptr && wl_ep != nullptr &&
       jit->GetCodeCache()->ContainsPc(wl_ep));
  if (!wl_has_jit_code && !method->IsNative() && method->GetCodeItem() != nullptr) {
    CodeItemDataAccessor accessor(method->DexInstructionData());
    ArtInterpreterToInterpreterBridge(self, accessor, shadow_frame, result);
    return;
  }
  const void* quick_entry = method->GetEntryPointFromQuickCompiledCode();
  if (UNLIKELY(PFCutPf625EntryLooksInvalid(quick_entry))) {
    PFCutPf625LogUnsafeNativeEntry(
        "compiled-bridge", method, caller, quick_entry, method->GetEntryPointFromJni());
    if (method->IsNative()) {
      method->SetEntryPointFromQuickCompiledCode(GetQuickGenericJniStub());
    } else if (method->GetCodeItem() != nullptr) {
      CodeItemDataAccessor accessor(method->DexInstructionData());
      ArtInterpreterToInterpreterBridge(self, accessor, shadow_frame, result);
      return;
    }
  }
  if (method->IsNative() &&
      method->IsStatic() &&
      WlSafeDescriptor(method) != nullptr &&
      strcmp(WlSafeDescriptor(method), "Ljava/util/TimeZone;") == 0 &&
      (strcmp(WlSafeName(method), "getDefault") == 0 ||
       strcmp(WlSafeName(method), "getDefaultRef") == 0) &&
      method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShortyView() == "L") {
    using FnType = jobject (*)(JNIEnv*, jclass);
    FnType fn = reinterpret_cast<FnType>(
        const_cast<void*>(method->GetEntryPointFromJni()));
    JNIEnvExt* env = down_cast<JNIEnvExt*>(self->GetJniEnv());
    const bool trace_tz = WestlakeTraceTimeZoneBridge();
    if (trace_tz) {
      fprintf(stderr,
              "[WESTLAKE-TZ-BRIDGE] direct native bridge method=%s fn=%p env=%p\n",
              method->PrettyMethod().c_str(),
              reinterpret_cast<void*>(fn),
              env);
      fflush(stderr);
    }
    jobject jresult = fn(env, nullptr);
    if (trace_tz) {
      fprintf(stderr, "[WESTLAKE-TZ-BRIDGE] direct native bridge result=%p\n", jresult);
      fflush(stderr);
    }
    result->SetL(self->DecodeJObject(jresult));
    if (trace_tz) {
      fprintf(stderr, "[WESTLAKE-TZ-BRIDGE] direct native bridge decoded\n");
      fflush(stderr);
    }
    return;
  }
  method->Invoke(self, shadow_frame->GetVRegArgs(arg_offset),
                 (shadow_frame->NumberOfVRegs() - arg_offset) * sizeof(uint32_t),
                 result, method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty());
}

void SetStringInitValueToAllAliases(ShadowFrame* shadow_frame,
                                    uint16_t this_obj_vreg,
                                    JValue result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Object> existing = shadow_frame->GetVRegReference(this_obj_vreg);
  if (existing == nullptr) {
    // If it's null, we come from compiled code that was deoptimized. Nothing to do,
    // as the compiler verified there was no alias.
    // Set the new string result of the StringFactory.
    shadow_frame->SetVRegReference(this_obj_vreg, result.GetL());
    return;
  }
  // Set the string init result into all aliases.
  for (uint32_t i = 0, e = shadow_frame->NumberOfVRegs(); i < e; ++i) {
    if (shadow_frame->GetVRegReference(i) == existing) {
      DCHECK_EQ(shadow_frame->GetVRegReference(i),
                reinterpret_cast32<mirror::Object*>(shadow_frame->GetVReg(i)));
      shadow_frame->SetVRegReference(i, result.GetL());
      DCHECK_EQ(shadow_frame->GetVRegReference(i),
                reinterpret_cast32<mirror::Object*>(shadow_frame->GetVReg(i)));
    }
  }
}

template<bool is_range>
static bool DoMethodHandleInvokeCommon(Thread* self,
                                       ShadowFrame& shadow_frame,
                                       bool invoke_exact,
                                       const Instruction* inst,
                                       uint16_t inst_data,
                                       JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Make sure to check for async exceptions
  if (UNLIKELY(self->ObserveAsyncException())) {
    return false;
  }
  // Invoke-polymorphic instructions always take a receiver. i.e, they are never static.
  const uint32_t vRegC = (is_range) ? inst->VRegC_4rcc() : inst->VRegC_45cc();
  const int invoke_method_idx = (is_range) ? inst->VRegB_4rcc() : inst->VRegB_45cc();

  // Initialize |result| to 0 as this is the default return value for
  // polymorphic invocations of method handle types with void return
  // and provides a sensible return result in error cases.
  result->SetJ(0);

  // The invoke_method_idx here is the name of the signature polymorphic method that
  // was symbolically invoked in bytecode (say MethodHandle.invoke or MethodHandle.invokeExact)
  // and not the method that we'll dispatch to in the end.
  StackHandleScope<2> hs(self);
  Handle<mirror::MethodHandle> method_handle(hs.NewHandle(
      ObjPtr<mirror::MethodHandle>::DownCast(shadow_frame.GetVRegReference(vRegC))));
  if (UNLIKELY(method_handle == nullptr)) {
    // Note that the invoke type is kVirtual here because a call to a signature
    // polymorphic method is shaped like a virtual call at the bytecode level.
    ThrowNullPointerExceptionForMethodAccess(invoke_method_idx, InvokeType::kVirtual);
    return false;
  }

  // The vRegH value gives the index of the proto_id associated with this
  // signature polymorphic call site.
  const uint16_t vRegH = (is_range) ? inst->VRegH_4rcc() : inst->VRegH_45cc();
  const dex::ProtoIndex callsite_proto_id(vRegH);

  // Call through to the classlinker and ask it to resolve the static type associated
  // with the callsite. This information is stored in the dex cache so it's
  // guaranteed to be fast after the first resolution.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::MethodType> callsite_type(hs.NewHandle(
      class_linker->ResolveMethodType(self, callsite_proto_id, shadow_frame.GetMethod())));

  // This implies we couldn't resolve one or more types in this method handle.
  if (UNLIKELY(callsite_type == nullptr)) {
    CHECK(self->IsExceptionPending());
    return false;
  }

  // There is a common dispatch method for method handles that takes
  // arguments either from a range or an array of arguments depending
  // on whether the DEX instruction is invoke-polymorphic/range or
  // invoke-polymorphic. The array here is for the latter.
  if (UNLIKELY(is_range)) {
    // VRegC is the register holding the method handle. Arguments passed
    // to the method handle's target do not include the method handle.
    RangeInstructionOperands operands(inst->VRegC_4rcc() + 1, inst->VRegA_4rcc() - 1);
    if (invoke_exact) {
      return MethodHandleInvokeExact(self,
                                     shadow_frame,
                                     method_handle,
                                     callsite_type,
                                     &operands,
                                     result);
    } else {
      return MethodHandleInvoke(self,
                                shadow_frame,
                                method_handle,
                                callsite_type,
                                &operands,
                                result);
    }
  } else {
    // Get the register arguments for the invoke.
    uint32_t args[Instruction::kMaxVarArgRegs] = {};
    inst->GetVarArgs(args, inst_data);
    // Drop the first register which is the method handle performing the invoke.
    memmove(args, args + 1, sizeof(args[0]) * (Instruction::kMaxVarArgRegs - 1));
    args[Instruction::kMaxVarArgRegs - 1] = 0;
    VarArgsInstructionOperands operands(args, inst->VRegA_45cc() - 1);
    if (invoke_exact) {
      return MethodHandleInvokeExact(self,
                                     shadow_frame,
                                     method_handle,
                                     callsite_type,
                                     &operands,
                                     result);
    } else {
      return MethodHandleInvoke(self,
                                shadow_frame,
                                method_handle,
                                callsite_type,
                                &operands,
                                result);
    }
  }
}

bool DoMethodHandleInvokeExact(Thread* self,
                               ShadowFrame& shadow_frame,
                               const Instruction* inst,
                               uint16_t inst_data,
                               JValue* result) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (inst->Opcode() == Instruction::INVOKE_POLYMORPHIC) {
    static const bool kIsRange = false;
    return DoMethodHandleInvokeCommon<kIsRange>(
        self, shadow_frame, /* invoke_exact= */ true, inst, inst_data, result);
  } else {
    DCHECK_EQ(inst->Opcode(), Instruction::INVOKE_POLYMORPHIC_RANGE);
    static const bool kIsRange = true;
    return DoMethodHandleInvokeCommon<kIsRange>(
        self, shadow_frame, /* invoke_exact= */ true, inst, inst_data, result);
  }
}

bool DoMethodHandleInvoke(Thread* self,
                          ShadowFrame& shadow_frame,
                          const Instruction* inst,
                          uint16_t inst_data,
                          JValue* result) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (inst->Opcode() == Instruction::INVOKE_POLYMORPHIC) {
    static const bool kIsRange = false;
    return DoMethodHandleInvokeCommon<kIsRange>(
        self, shadow_frame, /* invoke_exact= */ false, inst, inst_data, result);
  } else {
    DCHECK_EQ(inst->Opcode(), Instruction::INVOKE_POLYMORPHIC_RANGE);
    static const bool kIsRange = true;
    return DoMethodHandleInvokeCommon<kIsRange>(
        self, shadow_frame, /* invoke_exact= */ false, inst, inst_data, result);
  }
}

static bool DoVarHandleInvokeCommon(Thread* self,
                                    ShadowFrame& shadow_frame,
                                    const Instruction* inst,
                                    uint16_t inst_data,
                                    JValue* result,
                                    mirror::VarHandle::AccessMode access_mode)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Make sure to check for async exceptions
  if (UNLIKELY(self->ObserveAsyncException())) {
    return false;
  }

  bool is_var_args = inst->HasVarArgs();
  const uint32_t vRegC = is_var_args ? inst->VRegC_45cc() : inst->VRegC_4rcc();
  const uint16_t vRegH = is_var_args ? inst->VRegH_45cc() : inst->VRegH_4rcc();
  StackHandleScope<1> hs(self);
  Handle<mirror::VarHandle> var_handle = hs.NewHandle(
      ObjPtr<mirror::VarHandle>::DownCast(shadow_frame.GetVRegReference(vRegC)));
  ArtMethod* method = shadow_frame.GetMethod();
  uint32_t var_args[Instruction::kMaxVarArgRegs];
  std::optional<VarArgsInstructionOperands> var_args_operands(std::nullopt);
  std::optional<RangeInstructionOperands> range_operands(std::nullopt);
  InstructionOperands* all_operands;
  if (is_var_args) {
    inst->GetVarArgs(var_args, inst_data);
    var_args_operands.emplace(var_args, inst->VRegA_45cc());
    all_operands = &var_args_operands.value();
  } else {
    range_operands.emplace(inst->VRegC_4rcc(), inst->VRegA_4rcc());
    all_operands = &range_operands.value();
  }
  NoReceiverInstructionOperands operands(all_operands);

  return VarHandleInvokeAccessor(self,
                                 shadow_frame,
                                 var_handle,
                                 method,
                                 dex::ProtoIndex(vRegH),
                                 access_mode,
                                 &operands,
                                 result);
}

#define DO_VAR_HANDLE_ACCESSOR(_access_mode)                                                \
bool DoVarHandle ## _access_mode(Thread* self,                                              \
                                 ShadowFrame& shadow_frame,                                 \
                                 const Instruction* inst,                                   \
                                 uint16_t inst_data,                                        \
                                 JValue* result) REQUIRES_SHARED(Locks::mutator_lock_) {    \
  const auto access_mode = mirror::VarHandle::AccessMode::k ## _access_mode;                \
  return DoVarHandleInvokeCommon(self, shadow_frame, inst, inst_data, result, access_mode); \
}

DO_VAR_HANDLE_ACCESSOR(CompareAndExchange)
DO_VAR_HANDLE_ACCESSOR(CompareAndExchangeAcquire)
DO_VAR_HANDLE_ACCESSOR(CompareAndExchangeRelease)
DO_VAR_HANDLE_ACCESSOR(CompareAndSet)
DO_VAR_HANDLE_ACCESSOR(Get)
DO_VAR_HANDLE_ACCESSOR(GetAcquire)
DO_VAR_HANDLE_ACCESSOR(GetAndAdd)
DO_VAR_HANDLE_ACCESSOR(GetAndAddAcquire)
DO_VAR_HANDLE_ACCESSOR(GetAndAddRelease)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseAnd)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseAndAcquire)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseAndRelease)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseOr)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseOrAcquire)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseOrRelease)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseXor)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseXorAcquire)
DO_VAR_HANDLE_ACCESSOR(GetAndBitwiseXorRelease)
DO_VAR_HANDLE_ACCESSOR(GetAndSet)
DO_VAR_HANDLE_ACCESSOR(GetAndSetAcquire)
DO_VAR_HANDLE_ACCESSOR(GetAndSetRelease)
DO_VAR_HANDLE_ACCESSOR(GetOpaque)
DO_VAR_HANDLE_ACCESSOR(GetVolatile)
DO_VAR_HANDLE_ACCESSOR(Set)
DO_VAR_HANDLE_ACCESSOR(SetOpaque)
DO_VAR_HANDLE_ACCESSOR(SetRelease)
DO_VAR_HANDLE_ACCESSOR(SetVolatile)
DO_VAR_HANDLE_ACCESSOR(WeakCompareAndSet)
DO_VAR_HANDLE_ACCESSOR(WeakCompareAndSetAcquire)
DO_VAR_HANDLE_ACCESSOR(WeakCompareAndSetPlain)
DO_VAR_HANDLE_ACCESSOR(WeakCompareAndSetRelease)

#undef DO_VAR_HANDLE_ACCESSOR

template<bool is_range>
bool DoInvokePolymorphic(Thread* self,
                         ShadowFrame& shadow_frame,
                         const Instruction* inst,
                         uint16_t inst_data,
                         JValue* result) {
  const int invoke_method_idx = inst->VRegB();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ArtMethod* invoke_method =
      class_linker->ResolveMethod<ClassLinker::ResolveMode::kCheckICCEAndIAE>(
          self, invoke_method_idx, shadow_frame.GetMethod(), kPolymorphic);

  // Ensure intrinsic identifiers are initialized.
  DCHECK(invoke_method->IsIntrinsic());

  // Dispatch based on intrinsic identifier associated with method.
  switch (invoke_method->GetIntrinsic()) {
#define CASE_SIGNATURE_POLYMORPHIC_INTRINSIC(Name, ...) \
    case Intrinsics::k##Name:                           \
      return Do ## Name(self, shadow_frame, inst, inst_data, result);
    ART_SIGNATURE_POLYMORPHIC_INTRINSICS_LIST(CASE_SIGNATURE_POLYMORPHIC_INTRINSIC)
#undef CASE_SIGNATURE_POLYMORPHIC_INTRINSIC
    default:
      LOG(FATAL) << "Unreachable: " << invoke_method->GetIntrinsic();
      UNREACHABLE();
      return false;
  }
}

static JValue ConvertScalarBootstrapArgument(jvalue value) {
  // value either contains a primitive scalar value if it corresponds
  // to a primitive type, or it contains an integer value if it
  // corresponds to an object instance reference id (e.g. a string id).
  return JValue::FromPrimitive(value.j);
}

static ObjPtr<mirror::Class> GetClassForBootstrapArgument(EncodedArrayValueIterator::ValueType type)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ObjPtr<mirror::ObjectArray<mirror::Class>> class_roots = class_linker->GetClassRoots();
  switch (type) {
    case EncodedArrayValueIterator::ValueType::kBoolean:
    case EncodedArrayValueIterator::ValueType::kByte:
    case EncodedArrayValueIterator::ValueType::kChar:
    case EncodedArrayValueIterator::ValueType::kShort:
      // These types are disallowed by JVMS. Treat as integers. This
      // will result in CCE's being raised if the BSM has one of these
      // types.
    case EncodedArrayValueIterator::ValueType::kInt:
      return GetClassRoot(ClassRoot::kPrimitiveInt, class_roots);
    case EncodedArrayValueIterator::ValueType::kLong:
      return GetClassRoot(ClassRoot::kPrimitiveLong, class_roots);
    case EncodedArrayValueIterator::ValueType::kFloat:
      return GetClassRoot(ClassRoot::kPrimitiveFloat, class_roots);
    case EncodedArrayValueIterator::ValueType::kDouble:
      return GetClassRoot(ClassRoot::kPrimitiveDouble, class_roots);
    case EncodedArrayValueIterator::ValueType::kMethodType:
      return GetClassRoot<mirror::MethodType>(class_roots);
    case EncodedArrayValueIterator::ValueType::kMethodHandle:
      return GetClassRoot<mirror::MethodHandle>(class_roots);
    case EncodedArrayValueIterator::ValueType::kString:
      return GetClassRoot<mirror::String>();
    case EncodedArrayValueIterator::ValueType::kType:
      return GetClassRoot<mirror::Class>();
    case EncodedArrayValueIterator::ValueType::kField:
    case EncodedArrayValueIterator::ValueType::kMethod:
    case EncodedArrayValueIterator::ValueType::kEnum:
    case EncodedArrayValueIterator::ValueType::kArray:
    case EncodedArrayValueIterator::ValueType::kAnnotation:
    case EncodedArrayValueIterator::ValueType::kNull:
      return nullptr;
    case EncodedArrayValueIterator::ValueType::kEndOfInput:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

static bool GetArgumentForBootstrapMethod(Thread* self,
                                          ArtMethod* referrer,
                                          EncodedArrayValueIterator::ValueType type,
                                          const JValue* encoded_value,
                                          JValue* decoded_value)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // The encoded_value contains either a scalar value (IJDF) or a
  // scalar DEX file index to a reference type to be materialized.
  switch (type) {
    case EncodedArrayValueIterator::ValueType::kInt:
    case EncodedArrayValueIterator::ValueType::kFloat:
      decoded_value->SetI(encoded_value->GetI());
      return true;
    case EncodedArrayValueIterator::ValueType::kLong:
    case EncodedArrayValueIterator::ValueType::kDouble:
      decoded_value->SetJ(encoded_value->GetJ());
      return true;
    case EncodedArrayValueIterator::ValueType::kMethodType: {
      StackHandleScope<2> hs(self);
      Handle<mirror::ClassLoader> class_loader(hs.NewHandle(referrer->GetClassLoader()));
      Handle<mirror::DexCache> dex_cache(hs.NewHandle(referrer->GetDexCache()));
      dex::ProtoIndex proto_idx(encoded_value->GetC());
      ClassLinker* cl = Runtime::Current()->GetClassLinker();
      ObjPtr<mirror::MethodType> o =
          cl->ResolveMethodType(self, proto_idx, dex_cache, class_loader);
      if (UNLIKELY(o.IsNull())) {
        DCHECK(self->IsExceptionPending());
        return false;
      }
      decoded_value->SetL(o);
      return true;
    }
    case EncodedArrayValueIterator::ValueType::kMethodHandle: {
      uint32_t index = static_cast<uint32_t>(encoded_value->GetI());
      ClassLinker* cl = Runtime::Current()->GetClassLinker();
      ObjPtr<mirror::MethodHandle> o = cl->ResolveMethodHandle(self, index, referrer);
      if (UNLIKELY(o.IsNull())) {
        DCHECK(self->IsExceptionPending());
        return false;
      }
      decoded_value->SetL(o);
      return true;
    }
    case EncodedArrayValueIterator::ValueType::kString: {
      dex::StringIndex index(static_cast<uint32_t>(encoded_value->GetI()));
      ClassLinker* cl = Runtime::Current()->GetClassLinker();
      ObjPtr<mirror::String> o = cl->ResolveString(index, referrer);
      if (UNLIKELY(o.IsNull())) {
        DCHECK(self->IsExceptionPending());
        return false;
      }
      decoded_value->SetL(o);
      return true;
    }
    case EncodedArrayValueIterator::ValueType::kType: {
      dex::TypeIndex index(static_cast<uint32_t>(encoded_value->GetI()));
      ClassLinker* cl = Runtime::Current()->GetClassLinker();
      ObjPtr<mirror::Class> o = cl->ResolveType(index, referrer);
      if (UNLIKELY(o.IsNull())) {
        DCHECK(self->IsExceptionPending());
        return false;
      }
      decoded_value->SetL(o);
      return true;
    }
    case EncodedArrayValueIterator::ValueType::kBoolean:
    case EncodedArrayValueIterator::ValueType::kByte:
    case EncodedArrayValueIterator::ValueType::kChar:
    case EncodedArrayValueIterator::ValueType::kShort:
    case EncodedArrayValueIterator::ValueType::kField:
    case EncodedArrayValueIterator::ValueType::kMethod:
    case EncodedArrayValueIterator::ValueType::kEnum:
    case EncodedArrayValueIterator::ValueType::kArray:
    case EncodedArrayValueIterator::ValueType::kAnnotation:
    case EncodedArrayValueIterator::ValueType::kNull:
      // Unreachable - unsupported types that have been checked when
      // determining the effect call site type based on the bootstrap
      // argument types.
    case EncodedArrayValueIterator::ValueType::kEndOfInput:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

static bool PackArgumentForBootstrapMethod(Thread* self,
                                           ArtMethod* referrer,
                                           CallSiteArrayValueIterator* it,
                                           ShadowFrameSetter* setter)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  auto type = it->GetValueType();
  const JValue encoded_value = ConvertScalarBootstrapArgument(it->GetJavaValue());
  JValue decoded_value;
  if (!GetArgumentForBootstrapMethod(self, referrer, type, &encoded_value, &decoded_value)) {
    return false;
  }
  switch (it->GetValueType()) {
    case EncodedArrayValueIterator::ValueType::kInt:
    case EncodedArrayValueIterator::ValueType::kFloat:
      setter->Set(static_cast<uint32_t>(decoded_value.GetI()));
      return true;
    case EncodedArrayValueIterator::ValueType::kLong:
    case EncodedArrayValueIterator::ValueType::kDouble:
      setter->SetLong(decoded_value.GetJ());
      return true;
    case EncodedArrayValueIterator::ValueType::kMethodType:
    case EncodedArrayValueIterator::ValueType::kMethodHandle:
    case EncodedArrayValueIterator::ValueType::kString:
    case EncodedArrayValueIterator::ValueType::kType:
      setter->SetReference(decoded_value.GetL());
      return true;
    case EncodedArrayValueIterator::ValueType::kBoolean:
    case EncodedArrayValueIterator::ValueType::kByte:
    case EncodedArrayValueIterator::ValueType::kChar:
    case EncodedArrayValueIterator::ValueType::kShort:
    case EncodedArrayValueIterator::ValueType::kField:
    case EncodedArrayValueIterator::ValueType::kMethod:
    case EncodedArrayValueIterator::ValueType::kEnum:
    case EncodedArrayValueIterator::ValueType::kArray:
    case EncodedArrayValueIterator::ValueType::kAnnotation:
    case EncodedArrayValueIterator::ValueType::kNull:
      // Unreachable - unsupported types that have been checked when
      // determining the effect call site type based on the bootstrap
      // argument types.
    case EncodedArrayValueIterator::ValueType::kEndOfInput:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

static bool PackCollectorArrayForBootstrapMethod(Thread* self,
                                                 ArtMethod* referrer,
                                                 ObjPtr<mirror::Class> array_type,
                                                 int32_t array_length,
                                                 CallSiteArrayValueIterator* it,
                                                 ShadowFrameSetter* setter)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  JValue decoded_value;

#define COLLECT_PRIMITIVE_ARRAY(Descriptor, Type)                       \
  Handle<mirror::Type ## Array> array =                                 \
      hs.NewHandle(mirror::Type ## Array::Alloc(self, array_length));   \
  if (array.IsNull()) {                                                 \
    return false;                                                       \
  }                                                                     \
  for (int32_t i = 0; it->HasNext(); it->Next(), ++i) {                 \
    auto type = it->GetValueType();                                     \
    DCHECK_EQ(type, EncodedArrayValueIterator::ValueType::k ## Type);   \
    const JValue encoded_value =                                        \
        ConvertScalarBootstrapArgument(it->GetJavaValue());             \
    GetArgumentForBootstrapMethod(self,                                 \
                                  referrer,                             \
                                  type,                                 \
                                  &encoded_value,                       \
                                  &decoded_value);                      \
    array->Set(i, decoded_value.Get ## Descriptor());                   \
  }                                                                     \
  setter->SetReference(array.Get());                                    \
  return true;

#define COLLECT_REFERENCE_ARRAY(T, Type)                                \
  Handle<mirror::ObjectArray<T>> array =                   /* NOLINT */ \
      hs.NewHandle(mirror::ObjectArray<T>::Alloc(self,                  \
                                                 array_type,            \
                                                 array_length));        \
  if (array.IsNull()) {                                                 \
    return false;                                                       \
  }                                                                     \
  for (int32_t i = 0; it->HasNext(); it->Next(), ++i) {                 \
    auto type = it->GetValueType();                                     \
    DCHECK_EQ(type, EncodedArrayValueIterator::ValueType::k ## Type);   \
    const JValue encoded_value =                                        \
        ConvertScalarBootstrapArgument(it->GetJavaValue());             \
    if (!GetArgumentForBootstrapMethod(self,                            \
                                       referrer,                        \
                                       type,                            \
                                       &encoded_value,                  \
                                       &decoded_value)) {               \
      return false;                                                     \
    }                                                                   \
    ObjPtr<mirror::Object> o = decoded_value.GetL();                    \
    if (Runtime::Current()->IsActiveTransaction()) {                    \
      array->Set<true>(i, ObjPtr<T>::DownCast(o));                      \
    } else {                                                            \
      array->Set<false>(i, ObjPtr<T>::DownCast(o));                     \
    }                                                                   \
  }                                                                     \
  setter->SetReference(array.Get());                                    \
  return true;

  ObjPtr<mirror::ObjectArray<mirror::Class>> class_roots = class_linker->GetClassRoots();
  ObjPtr<mirror::Class> component_type = array_type->GetComponentType();
  if (component_type == GetClassRoot(ClassRoot::kPrimitiveInt, class_roots)) {
    COLLECT_PRIMITIVE_ARRAY(I, Int);
  } else if (component_type == GetClassRoot(ClassRoot::kPrimitiveLong, class_roots)) {
    COLLECT_PRIMITIVE_ARRAY(J, Long);
  } else if (component_type == GetClassRoot(ClassRoot::kPrimitiveFloat, class_roots)) {
    COLLECT_PRIMITIVE_ARRAY(F, Float);
  } else if (component_type == GetClassRoot(ClassRoot::kPrimitiveDouble, class_roots)) {
    COLLECT_PRIMITIVE_ARRAY(D, Double);
  } else if (component_type == GetClassRoot<mirror::MethodType>()) {
    COLLECT_REFERENCE_ARRAY(mirror::MethodType, MethodType);
  } else if (component_type == GetClassRoot<mirror::MethodHandle>()) {
    COLLECT_REFERENCE_ARRAY(mirror::MethodHandle, MethodHandle);
  } else if (component_type == GetClassRoot<mirror::String>(class_roots)) {
    COLLECT_REFERENCE_ARRAY(mirror::String, String);
  } else if (component_type == GetClassRoot<mirror::Class>()) {
    COLLECT_REFERENCE_ARRAY(mirror::Class, Type);
  } else {
    component_type->DumpClass(LOG_STREAM(FATAL_WITHOUT_ABORT), mirror::Class::kDumpClassFullDetail);
    LOG(FATAL) << "unexpected class: " << component_type->PrettyTypeOf();
    UNREACHABLE();
  }
  #undef COLLECT_PRIMITIVE_ARRAY
  #undef COLLECT_REFERENCE_ARRAY
}

static ObjPtr<mirror::MethodType> BuildCallSiteForBootstrapMethod(Thread* self,
                                                                  const DexFile* dex_file,
                                                                  uint32_t call_site_idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const dex::CallSiteIdItem& csi = dex_file->GetCallSiteId(call_site_idx);
  CallSiteArrayValueIterator it(*dex_file, csi);
  DCHECK_GE(it.Size(), 1u);

  StackHandleScope<2> hs(self);
  // Create array for parameter types.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ObjPtr<mirror::Class> class_array_type =
      GetClassRoot<mirror::ObjectArray<mirror::Class>>(class_linker);
  Handle<mirror::ObjectArray<mirror::Class>> ptypes = hs.NewHandle(
      mirror::ObjectArray<mirror::Class>::Alloc(self,
                                                class_array_type,
                                                static_cast<int>(it.Size())));
  if (ptypes.IsNull()) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }

  // Populate the first argument with an instance of j.l.i.MethodHandles.Lookup
  // that the runtime will construct.
  ptypes->Set(0, GetClassRoot<mirror::MethodHandlesLookup>(class_linker));
  it.Next();

  // The remaining parameter types are derived from the types of
  // arguments present in the DEX file.
  int index = 1;
  while (it.HasNext()) {
    ObjPtr<mirror::Class> ptype = GetClassForBootstrapArgument(it.GetValueType());
    if (ptype.IsNull()) {
      ThrowClassCastException("Unsupported bootstrap argument type");
      return nullptr;
    }
    ptypes->Set(index, ptype);
    index++;
    it.Next();
  }
  DCHECK_EQ(static_cast<size_t>(index), it.Size());

  // By definition, the return type is always a j.l.i.CallSite.
  Handle<mirror::Class> rtype = hs.NewHandle(GetClassRoot<mirror::CallSite>());
  return mirror::MethodType::Create(self, rtype, ptypes);
}

static ObjPtr<mirror::CallSite> InvokeBootstrapMethod(Thread* self,
                                                      ShadowFrame& shadow_frame,
                                                      uint32_t call_site_idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<5> hs(self);
  // There are three mandatory arguments expected from the call site
  // value array in the DEX file: the bootstrap method handle, the
  // method name to pass to the bootstrap method, and the method type
  // to pass to the bootstrap method.
  static constexpr size_t kMandatoryArgumentsCount = 3;
  ArtMethod* referrer = shadow_frame.GetMethod();
  const DexFile* dex_file = referrer->GetDexFile();
  const dex::CallSiteIdItem& csi = dex_file->GetCallSiteId(call_site_idx);
  CallSiteArrayValueIterator it(*dex_file, csi);
  if (it.Size() < kMandatoryArgumentsCount) {
    ThrowBootstrapMethodError("Truncated bootstrap arguments (%zu < %zu)",
                              it.Size(), kMandatoryArgumentsCount);
    return nullptr;
  }

  if (it.GetValueType() != EncodedArrayValueIterator::ValueType::kMethodHandle) {
    ThrowBootstrapMethodError("First bootstrap argument is not a method handle");
    return nullptr;
  }

  uint32_t bsm_index = static_cast<uint32_t>(it.GetJavaValue().i);
  it.Next();

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::MethodHandle> bsm =
      hs.NewHandle(class_linker->ResolveMethodHandle(self, bsm_index, referrer));
  if (bsm.IsNull()) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }

  if (bsm->GetHandleKind() != mirror::MethodHandle::Kind::kInvokeStatic) {
    // JLS suggests also accepting constructors. This is currently
    // hard as constructor invocations happen via transformers in ART
    // today. The constructor would need to be a class derived from java.lang.invoke.CallSite.
    ThrowBootstrapMethodError("Unsupported bootstrap method invocation kind");
    return nullptr;
  }

  // Construct the local call site type information based on the 3
  // mandatory arguments provided by the runtime and the static arguments
  // in the DEX file. We will use these arguments to build a shadow frame.
  MutableHandle<mirror::MethodType> call_site_type =
      hs.NewHandle(BuildCallSiteForBootstrapMethod(self, dex_file, call_site_idx));
  if (call_site_type.IsNull()) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }

  // Check if this BSM is targeting a variable arity method. If so,
  // we'll need to collect the trailing arguments into an array.
  Handle<mirror::Array> collector_arguments;
  int32_t collector_arguments_length;
  if (bsm->GetTargetMethod()->IsVarargs()) {
    int number_of_bsm_parameters = bsm->GetMethodType()->GetNumberOfPTypes();
    if (number_of_bsm_parameters == 0) {
      ThrowBootstrapMethodError("Variable arity BSM does not have any arguments");
      return nullptr;
    }
    Handle<mirror::Class> collector_array_class =
        hs.NewHandle(bsm->GetMethodType()->GetPTypes()->Get(number_of_bsm_parameters - 1));
    if (!collector_array_class->IsArrayClass()) {
      ThrowBootstrapMethodError("Variable arity BSM does not have array as final argument");
      return nullptr;
    }
    // The call site may include no arguments to be collected. In this
    // case the number of arguments must be at least the number of BSM
    // parameters less the collector array.
    if (call_site_type->GetNumberOfPTypes() < number_of_bsm_parameters - 1) {
      ThrowWrongMethodTypeException(bsm->GetMethodType(), call_site_type.Get());
      return nullptr;
    }
    // Check all the arguments to be collected match the collector array component type.
    for (int i = number_of_bsm_parameters - 1; i < call_site_type->GetNumberOfPTypes(); ++i) {
      if (call_site_type->GetPTypes()->Get(i) != collector_array_class->GetComponentType()) {
        ThrowClassCastException(collector_array_class->GetComponentType(),
                                call_site_type->GetPTypes()->Get(i));
        return nullptr;
      }
    }
    // Update the call site method type so it now includes the collector array.
    int32_t collector_arguments_start = number_of_bsm_parameters - 1;
    collector_arguments_length = call_site_type->GetNumberOfPTypes() - number_of_bsm_parameters + 1;
    call_site_type.Assign(
        mirror::MethodType::CollectTrailingArguments(self,
                                                     call_site_type.Get(),
                                                     collector_array_class.Get(),
                                                     collector_arguments_start));
    if (call_site_type.IsNull()) {
      DCHECK(self->IsExceptionPending());
      return nullptr;
    }
  } else {
    collector_arguments_length = 0;
  }

  if (call_site_type->GetNumberOfPTypes() != bsm->GetMethodType()->GetNumberOfPTypes()) {
    ThrowWrongMethodTypeException(bsm->GetMethodType(), call_site_type.Get());
    return nullptr;
  }

  // BSM invocation has a different set of exceptions that
  // j.l.i.MethodHandle.invoke(). Scan arguments looking for CCE
  // "opportunities". Unfortunately we cannot just leave this to the
  // method handle invocation as this might generate a WMTE.
  for (int32_t i = 0; i < call_site_type->GetNumberOfPTypes(); ++i) {
    ObjPtr<mirror::Class> from = call_site_type->GetPTypes()->Get(i);
    ObjPtr<mirror::Class> to = bsm->GetMethodType()->GetPTypes()->Get(i);
    if (!IsParameterTypeConvertible(from, to)) {
      ThrowClassCastException(from, to);
      return nullptr;
    }
  }
  if (!IsReturnTypeConvertible(call_site_type->GetRType(), bsm->GetMethodType()->GetRType())) {
    ThrowClassCastException(bsm->GetMethodType()->GetRType(), call_site_type->GetRType());
    return nullptr;
  }

  // Set-up a shadow frame for invoking the bootstrap method handle.
  ShadowFrameAllocaUniquePtr bootstrap_frame =
      CREATE_SHADOW_FRAME(call_site_type->NumberOfVRegs(),
                          referrer,
                          shadow_frame.GetDexPC());
  ScopedStackedShadowFramePusher pusher(self, bootstrap_frame.get());
  ShadowFrameSetter setter(bootstrap_frame.get(), 0u);

  // The first parameter is a MethodHandles lookup instance.
  Handle<mirror::Class> lookup_class =
      hs.NewHandle(shadow_frame.GetMethod()->GetDeclaringClass());
  ObjPtr<mirror::MethodHandlesLookup> lookup =
      mirror::MethodHandlesLookup::Create(self, lookup_class);
  if (lookup.IsNull()) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }
  setter.SetReference(lookup);

  // Pack the remaining arguments into the frame.
  int number_of_arguments = call_site_type->GetNumberOfPTypes();
  int argument_index;
  for (argument_index = 1; argument_index < number_of_arguments; ++argument_index) {
    if (argument_index == number_of_arguments - 1 &&
        call_site_type->GetPTypes()->Get(argument_index)->IsArrayClass()) {
      ObjPtr<mirror::Class> array_type = call_site_type->GetPTypes()->Get(argument_index);
      if (!PackCollectorArrayForBootstrapMethod(self,
                                                referrer,
                                                array_type,
                                                collector_arguments_length,
                                                &it,
                                                &setter)) {
        DCHECK(self->IsExceptionPending());
        return nullptr;
      }
    } else if (!PackArgumentForBootstrapMethod(self, referrer, &it, &setter)) {
      DCHECK(self->IsExceptionPending());
      return nullptr;
    }
    it.Next();
  }
  DCHECK(!it.HasNext());
  DCHECK(setter.Done());

  // Invoke the bootstrap method handle.
  JValue result;
  RangeInstructionOperands operands(0, bootstrap_frame->NumberOfVRegs());
  bool invoke_success = MethodHandleInvoke(self,
                                           *bootstrap_frame,
                                           bsm,
                                           call_site_type,
                                           &operands,
                                           &result);
  if (!invoke_success) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }

  Handle<mirror::Object> object(hs.NewHandle(result.GetL()));
  if (UNLIKELY(object.IsNull())) {
    // This will typically be for LambdaMetafactory which is not supported.
    ThrowClassCastException("Bootstrap method returned null");
    return nullptr;
  }

  // Check the result type is a subclass of j.l.i.CallSite.
  ObjPtr<mirror::Class> call_site_class = GetClassRoot<mirror::CallSite>(class_linker);
  if (UNLIKELY(!object->InstanceOf(call_site_class))) {
    ThrowClassCastException(object->GetClass(), call_site_class);
    return nullptr;
  }

  // Check the call site target is not null as we're going to invoke it.
  ObjPtr<mirror::CallSite> call_site = ObjPtr<mirror::CallSite>::DownCast(result.GetL());
  ObjPtr<mirror::MethodHandle> target = call_site->GetTarget();
  if (UNLIKELY(target == nullptr)) {
    ThrowClassCastException("Bootstrap method returned a CallSite with a null target");
    return nullptr;
  }
  return call_site;
}

namespace {

ObjPtr<mirror::CallSite> DoResolveCallSite(Thread* self,
                                           ShadowFrame& shadow_frame,
                                           uint32_t call_site_idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(shadow_frame.GetMethod()->GetDexCache()));

  // Get the call site from the DexCache if present.
  ObjPtr<mirror::CallSite> call_site = dex_cache->GetResolvedCallSite(call_site_idx);
  if (LIKELY(call_site != nullptr)) {
    return call_site;
  }

  // Invoke the bootstrap method to get a candidate call site.
  call_site = InvokeBootstrapMethod(self, shadow_frame, call_site_idx);
  if (UNLIKELY(call_site == nullptr)) {
    if (!self->GetException()->IsError()) {
      // Use a BootstrapMethodError if the exception is not an instance of java.lang.Error.
      ThrowWrappedBootstrapMethodError("Exception from call site #%u bootstrap method",
                                       call_site_idx);
    }
    return nullptr;
  }

  // Attempt to place the candidate call site into the DexCache, return the winning call site.
  return dex_cache->SetResolvedCallSite(call_site_idx, call_site);
}

}  // namespace

bool DoInvokeCustom(Thread* self,
                    ShadowFrame& shadow_frame,
                    uint32_t call_site_idx,
                    const InstructionOperands* operands,
                    JValue* result) {
  // Make sure to check for async exceptions
  if (UNLIKELY(self->ObserveAsyncException())) {
    return false;
  }

  // invoke-custom is not supported in transactions. In transactions
  // there is a limited set of types supported. invoke-custom allows
  // running arbitrary code and instantiating arbitrary types.
  CHECK(!Runtime::Current()->IsActiveTransaction());

  ObjPtr<mirror::CallSite> call_site = DoResolveCallSite(self, shadow_frame, call_site_idx);
  if (call_site.IsNull()) {
    DCHECK(self->IsExceptionPending());
    return false;
  }

  StackHandleScope<2> hs(self);
  Handle<mirror::MethodHandle> target = hs.NewHandle(call_site->GetTarget());
  Handle<mirror::MethodType> target_method_type = hs.NewHandle(target->GetMethodType());
  DCHECK_EQ(operands->GetNumberOfOperands(), target_method_type->NumberOfVRegs())
      << " call_site_idx" << call_site_idx;
  return MethodHandleInvokeExact(self,
                                 shadow_frame,
                                 target,
                                 target_method_type,
                                 operands,
                                 result);
}

// Assign register 'src_reg' from shadow_frame to register 'dest_reg' into new_shadow_frame.
static inline void AssignRegister(ShadowFrame* new_shadow_frame, const ShadowFrame& shadow_frame,
                                  size_t dest_reg, size_t src_reg)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Uint required, so that sign extension does not make this wrong on 64b systems
  uint32_t src_value = shadow_frame.GetVReg(src_reg);
  ObjPtr<mirror::Object> o = shadow_frame.GetVRegReference<kVerifyNone>(src_reg);

  // If both register locations contains the same value, the register probably holds a reference.
  // Note: As an optimization, non-moving collectors leave a stale reference value
  // in the references array even after the original vreg was overwritten to a non-reference.
  if (src_value == reinterpret_cast32<uint32_t>(o.Ptr())) {
    new_shadow_frame->SetVRegReference(dest_reg, o);
  } else {
    new_shadow_frame->SetVReg(dest_reg, src_value);
  }
}

template <bool is_range>
inline void CopyRegisters(ShadowFrame& caller_frame,
                          ShadowFrame* callee_frame,
                          const uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                          const size_t first_src_reg,
                          const size_t first_dest_reg,
                          const size_t num_regs) {
  if (is_range) {
    const size_t dest_reg_bound = first_dest_reg + num_regs;
    for (size_t src_reg = first_src_reg, dest_reg = first_dest_reg; dest_reg < dest_reg_bound;
        ++dest_reg, ++src_reg) {
      AssignRegister(callee_frame, caller_frame, dest_reg, src_reg);
    }
  } else {
    DCHECK_LE(num_regs, arraysize(arg));

    for (size_t arg_index = 0; arg_index < num_regs; ++arg_index) {
      AssignRegister(callee_frame, caller_frame, first_dest_reg + arg_index, arg[arg_index]);
    }
  }
}

template <bool is_range>
static inline bool DoCallCommon(ArtMethod* called_method,
                                Thread* self,
                                ShadowFrame& shadow_frame,
                                JValue* result,
                                uint16_t number_of_inputs,
                                uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                uint32_t vregC,
                                bool string_init) {
  ArtMethod* caller_method = shadow_frame.GetMethod();
  // WESTLAKE §691: §690 proved KtR.h() does not unwind with an exception even though the mapped
  // WebView remains url=""/started=0.  Record its resolved callees so we can distinguish a skipped
  // loadDataWithBaseURL invoke from a framework/provider call that executes as a no-op.
  if (caller_method != nullptr && called_method != nullptr) {
    const std::string wl_caller = caller_method->PrettyMethod();
    if (wl_caller.find(" X.KtR.h(") != std::string::npos) {
      static thread_local int wl_kt_r_call_count = 0;
      if (wl_kt_r_call_count < 240) {
        ++wl_kt_r_call_count;
        fprintf(stderr, "[WESTLAKE-KTR-CALL] dex_pc=%u callee=%s\n",
                shadow_frame.GetDexPC(), called_method->PrettyMethod().c_str());
        fflush(stderr);
      }
    }
  }
  // WESTLAKE 2026-07-22: the app SIGSEGVs here with an ArtMethod* whose value is ASCII text
  // ("on/icon", fault addr 0x006e6f63692f6e6f). The faultlog is 96 frames of pure interpreter
  // internals and cannot name the Java method, so identify it HERE. Inspect the POINTER VALUE
  // ONLY -- never dereference called_method when it is structurally invalid.
  //
  // 2026-08-24: do not classify a pointer merely because its little-endian address bytes happen
  // to be printable.  Valid, mapped, aligned ArtMethod pointers such as 0x7e5b350050 triggered
  // that heuristic under some ASLR layouts and produced >10k false BADCALL/BADMETHOD pairs.
  // The original "on/icon" value is both unaligned and non-canonical, so the structural test
  // retains the crash guard without treating ASLR luck as memory corruption.
  {
    const uintptr_t m = reinterpret_cast<uintptr_t>(called_method);
    // a real ArtMethod* on this board lives well below 1<<48 and is 4-byte aligned
    const bool insane = (m & 0x3) != 0 || (m >> 48) != 0;
    if (m != 0 && insane) {
      char txt[9];
      for (int i = 0; i < 8; ++i) {
        const unsigned b = static_cast<unsigned>((m >> (i * 8)) & 0xff);
        txt[i] = (b >= 0x20 && b <= 0x7e) ? static_cast<char>(b) : '.';
      }
      txt[8] = '\0';
      fprintf(stderr,
              "[WESTLAKE-BADMETHOD] corrupt called_method=%p ascii=\"%s\" caller=%s dex_pc=%u "
              "inputs=%u vregC=%u string_init=%d\n",
              called_method, txt,
              caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
              shadow_frame.GetDexPC(), number_of_inputs, vregC, string_init ? 1 : 0);
      fflush(stderr);
    }
  }
  const bool pfc_l3_trace = PFCutIsL3OnCreate(caller_method);
  const bool pfc_diag = false;
  UNUSED(pfc_l3_trace);

	  if (pfc_diag) {
	    fprintf(stderr,
	            "[PFCUT] DoCallCommon enter dexIdx=%u called_method=%p caller=%p flags=0x%x native=%d abstract=%d copied=%d default=%d miranda=%d quick=%p jni=%p code_item=%p inputs=%u vregC=%u string_init=%d top_shadow=%p top_quick=%p\n",
            called_method->GetDexMethodIndex(),
            called_method,
            shadow_frame.GetMethod(),
            called_method->GetAccessFlags(),
            called_method->IsNative() ? 1 : 0,
            called_method->IsAbstract() ? 1 : 0,
            called_method->IsCopied() ? 1 : 0,
            called_method->IsDefault() ? 1 : 0,
            called_method->IsMiranda() ? 1 : 0,
            called_method->GetEntryPointFromQuickCompiledCode(),
            called_method->IsNative() ? called_method->GetEntryPointFromJni() : nullptr,
            called_method->GetCodeItem(),
            number_of_inputs,
            vregC,
            string_init ? 1 : 0,
            self->GetManagedStack()->GetTopShadowFrame(),
            self->GetManagedStack()->GetTopQuickFrame());
	    fflush(stderr);
	  }
	  if (!string_init && called_method != nullptr && called_method->IsProxyMethod()) {
	    static thread_local int proxy_docall_count = 0;
	    if (proxy_docall_count < 60) {
	      proxy_docall_count++;
	      fprintf(stderr,
	              "[PFCUT-PROXY] DoCallCommon proxy enter %s inputs=%u range=%d vregC=%u arg0=%u\n",
	              called_method->PrettyMethod().c_str(),
	              number_of_inputs,
	              is_range ? 1 : 0,
	              vregC,
	              is_range ? vregC : arg[0]);
	      fflush(stderr);
	    }
	  }
	  // Depth guard disabled — was corrupting caller's ShadowFrame vregs
  // TODO: investigate alloca interaction with thread_local + RAII

  if (!string_init) {
    PFCutLogKotlinResultFailure<is_range>(
        called_method, shadow_frame, number_of_inputs, arg, vregC);
  }

  if (!string_init) {
    PFCutNormalizeZipFileNullCharsetArg<is_range>(
        called_method, self, shadow_frame, number_of_inputs, arg, vregC);
  }

  if (!string_init &&
      PFCutTryAtomicReferenceArrayIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAtomicReferenceIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAtomicIntegerOrBooleanIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAtomicLongIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryUnixFileSystemListIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryUnixFileSystemGetBooleanAttributesIntrinsic<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryUnixFileSystemGetLengthIntrinsic<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryUnixFileSystemCanonicalizeIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryLinuxOpenIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryClassGetDeclaredFieldIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryClassNewInstanceIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryMethodHandlesLookupIntrinsic(called_method, self, shadow_frame, result)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryDexPathListFindLibraryIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryRealmNativeBoundaryNoop<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAndroidxWorkManagerConstructorLite<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryMcdLoggerNoop<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init && PFCutTryMcdJustFlipEventNoop(called_method, result)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryMcdPerfAnalyticsNoop<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryMcdNetworkBoundaryNoop<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryKotlinReflectionFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAndroidxLifecycleFactoryDefaultCreateBroadFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryHiltViewModelFactoryCreateFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAndroidxLifecycleKClassFactoryFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAndroidxLifecycleKClassHelperFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAndroidxLifecycleClassFactoryFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init && PFCutTryAndroidxSplashNoop(called_method, result)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryAndroidxLifecycleGeneratedAdapterFallback(
          called_method, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryNewRelicNoop<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryKotlinCoroutineExceptionNoop(called_method, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryProxyInvoke<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryStringFactoryCharsetFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryCharsetNameFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryZipFileRequireNonNullCharsetFallback<is_range>(
          called_method, caller_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryThreadGroupUncaughtExceptionNoop<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryIcuDataPathPropertyFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryVMClassLoaderGetResourceFallback(called_method, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryVMClassLoaderGetResourcesFallback(called_method, self, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryNumberFormatCurrencyFallback(called_method, self, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryCurrencyInstanceFallback(called_method, self, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryCurrencyMethodFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryULocaleForLocaleFallback(called_method, self, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryULocaleGetDefaultFallback(called_method, self, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryFileSystemsGetDefaultFallback(called_method, self, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryCharsetCodingErrorActionFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryUnixNativeDispatcherPathIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTrySystemTimeIntrinsic(called_method, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTrySystemArraycopyIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryRuntimeAvailableProcessorsIntrinsic<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryReflectUtilEnsureMemberAccessNoop(called_method, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryUuidIntrinsic(called_method, self, result, number_of_inputs)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryStringIntrinsic<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryGsonNextQuotedValue<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryGsonLinkedTreeMapPut<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  if (!string_init &&
      PFCutTryGsonNextNonWhitespace<is_range>(
          called_method, shadow_frame, result, number_of_inputs, arg, vregC)) {
    return !self->IsExceptionPending();
  }

  // TRACE at DoCallCommon entry
  {
    static thread_local int trace_count = 0;
    ArtMethod* cm = shadow_frame.GetMethod();
    if (cm && shadow_frame.NumberOfVRegs() <= 4) {
      const char* cn = WlSafeName(cm);
      if (cn && strcmp(cn, "lastIndexOf") == 0 && trace_count < 5) {
        trace_count++;
        int nr = shadow_frame.NumberOfVRegs();
        fprintf(stderr, "[VREG-ENTRY] DoCallCommon from %s → %s: ",
                cm->PrettyMethod().c_str(), called_method->PrettyMethod().c_str());
        for (int i = 0; i < nr; i++) fprintf(stderr, "v%d=0x%x ", i, shadow_frame.GetVReg(i));
        fprintf(stderr, "\n"); fflush(stderr);
      }
    }
  }
  // Compute method information.
  CodeItemDataAccessor accessor(called_method->DexInstructionData());
  if (pfc_diag) {
    fprintf(stderr,
            "[PFCUT] DoCallCommon after accessor dexIdx=%u has_code=%d native=%d proxy=%d\n",
            called_method->GetDexMethodIndex(),
            accessor.HasCodeItem() ? 1 : 0,
            called_method->IsNative() ? 1 : 0,
            called_method->IsProxyMethod() ? 1 : 0);
    fflush(stderr);
  }
  // Number of registers for the callee's call frame.
  uint16_t num_regs;
  // Test whether to use the interpreter or compiler entrypoint, and save that result to pass to
  // PerformCall. A deoptimization could occur at any time, and we shouldn't change which
  // entrypoint to use once we start building the shadow frame.

  bool use_interpreter_entrypoint = ShouldStayInSwitchInterpreter(called_method);
  bool pfc_had_stale_native_quick_entry = false;
  const void* quick_entry = called_method->GetEntryPointFromQuickCompiledCode();
  if (UNLIKELY(PFCutPf625EntryLooksInvalid(quick_entry))) {
    PFCutPf625LogUnsafeNativeEntry(
        "docall-preframe", called_method, caller_method, quick_entry,
        called_method->GetEntryPointFromJni());
  if (called_method->IsNative()) {
      pfc_had_stale_native_quick_entry = true;
      called_method->SetEntryPointFromQuickCompiledCode(GetQuickGenericJniStub());
      called_method->SetEntryPointFromJniPtrSize(nullptr, kRuntimePointerSize);
    } else {
      called_method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
      use_interpreter_entrypoint = true;
    }
  }
  if (!called_method->IsNative() && accessor.HasCodeItem()) {
    use_interpreter_entrypoint = true;
  }
  if (pfc_diag) {
    fprintf(stderr,
            "[PFCUT] DoCallCommon after ShouldStayInSwitchInterpreter dexIdx=%u use_interpreter=%d\n",
            called_method->GetDexMethodIndex(),
            use_interpreter_entrypoint ? 1 : 0);
    fflush(stderr);
  }
  if (LIKELY(accessor.HasCodeItem())) {
    // When transitioning to compiled code, space only needs to be reserved for the input registers.
    // The rest of the frame gets discarded. This also prevents accessing the called method's code
    // item, saving memory by keeping code items of compiled code untouched.
    if (!use_interpreter_entrypoint) {
      DCHECK(!Runtime::Current()->IsAotCompiler()) << "Compiler should use interpreter entrypoint";
      num_regs = number_of_inputs;
    } else {
      num_regs = accessor.RegistersSize();
      DCHECK_EQ(string_init ? number_of_inputs - 1 : number_of_inputs, accessor.InsSize());
    }
  } else {
    DCHECK(called_method->IsNative() || called_method->IsProxyMethod());
    num_regs = number_of_inputs;
  }

  // Hack for String init:
  //
  // Rewrite invoke-x java.lang.String.<init>(this, a, b, c, ...) into:
  //         invoke-x StringFactory(a, b, c, ...)
  // by effectively dropping the first virtual register from the invoke.
  //
  // (at this point the ArtMethod has already been replaced,
  // so we just need to fix-up the arguments)
  //
  // Note that FindMethodFromCode in entrypoint_utils-inl.h was also special-cased
  // to handle the compiler optimization of replacing `this` with null without
  // throwing NullPointerException.
  uint32_t string_init_vreg_this = is_range ? vregC : arg[0];
  if (UNLIKELY(string_init)) {
    DCHECK_GT(num_regs, 0u);  // As the method is an instance method, there should be at least 1.

    // The new StringFactory call is static and has one fewer argument.
    if (!accessor.HasCodeItem()) {
      DCHECK(called_method->IsNative() || called_method->IsProxyMethod());
      num_regs--;
    }  // else ... don't need to change num_regs since it comes up from the string_init's code item
    number_of_inputs--;

    // Rewrite the var-args, dropping the 0th argument ("this")
    for (uint32_t i = 1; i < arraysize(arg); ++i) {
      arg[i - 1] = arg[i];
    }
    arg[arraysize(arg) - 1] = 0;

    // Rewrite the non-var-arg case
    vregC++;  // Skips the 0th vreg in the range ("this").
  }

  if (UNLIKELY(string_init) &&
      PFCutTryStringFactoryCharsetFallback<is_range>(
          called_method, self, shadow_frame, result, number_of_inputs, arg, vregC)) {
    if (!self->IsExceptionPending()) {
      SetStringInitValueToAllAliases(&shadow_frame, string_init_vreg_this, *result);
    }
    return !self->IsExceptionPending();
  }

  // Parameter registers go at the end of the shadow frame.
  DCHECK_GE(num_regs, number_of_inputs);
  size_t first_dest_reg = num_regs - number_of_inputs;
  DCHECK_NE(first_dest_reg, (size_t)-1);
  if (pfc_diag) {
    fprintf(stderr,
            "[PFCUT] DoCallCommon before CREATE_SHADOW_FRAME dexIdx=%u num_regs=%u first_dest_reg=%zu\n",
            called_method->GetDexMethodIndex(),
            num_regs,
            first_dest_reg);
    fflush(stderr);
  }

  // Allocate shadow frame on the stack.
  const char* old_cause = self->StartAssertNoThreadSuspension("DoCallCommon");
  ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
      CREATE_SHADOW_FRAME(num_regs, called_method, /* dex pc */ 0);
  ShadowFrame* new_shadow_frame = shadow_frame_unique_ptr.get();
  if (pfc_diag) {
    fprintf(stderr,
            "[PFCUT] DoCallCommon after CREATE_SHADOW_FRAME dexIdx=%u new_shadow=%p old_cause=%p\n",
            called_method->GetDexMethodIndex(),
            new_shadow_frame,
            old_cause);
    fflush(stderr);
  }

  // Initialize new shadow frame by copying the registers from the callee shadow frame.
  if (!shadow_frame.GetMethod()->SkipAccessChecks()) {
    if (pfc_diag) {
      fprintf(stderr,
              "[PFCUT] DoCallCommon before checked arg copy dexIdx=%u caller_skip=0\n",
              called_method->GetDexMethodIndex());
      fflush(stderr);
    }
    // Slow path.
    // We might need to do class loading, which incurs a thread state change to kNative. So
    // register the shadow frame as under construction and allow suspension again.
    ScopedStackedShadowFramePusher pusher(self, new_shadow_frame);
    self->EndAssertNoThreadSuspension(old_cause);

    // ArtMethod here is needed to check type information of the call site against the callee.
    // Type information is retrieved from a DexFile/DexCache for that respective declared method.
    //
    // As a special case for proxy methods, which are not dex-backed,
    // we have to retrieve type information from the proxy's method
    // interface method instead (which is dex backed since proxies are never interfaces).
    ArtMethod* method =
        new_shadow_frame->GetMethod()->GetInterfaceMethodIfProxy(kRuntimePointerSize);

    // We need to do runtime check on reference assignment. We need to load the shorty
    // to get the exact type of each reference argument.
    const dex::TypeList* params = method->GetParameterTypeList();
    uint32_t shorty_len = 0;
    const char* shorty = method->GetShorty(&shorty_len);

    // Handle receiver apart since it's not part of the shorty.
    size_t dest_reg = first_dest_reg;
    size_t arg_offset = 0;

    if (!method->IsStatic()) {
      size_t receiver_reg = is_range ? vregC : arg[0];
      new_shadow_frame->SetVRegReference(dest_reg, shadow_frame.GetVRegReference(receiver_reg));
      ++dest_reg;
      ++arg_offset;
      DCHECK(!string_init);  // All StringFactory methods are static.
    }

    // Copy the caller's invoke-* arguments into the callee's parameter registers.
    for (uint32_t shorty_pos = 0; dest_reg < num_regs; ++shorty_pos, ++dest_reg, ++arg_offset) {
      // Skip the 0th 'shorty' type since it represents the return type.
      DCHECK_LT(shorty_pos + 1, shorty_len) << "for shorty '" << shorty << "'";
      const size_t src_reg = (is_range) ? vregC + arg_offset : arg[arg_offset];
      switch (shorty[shorty_pos + 1]) {
        // Handle Object references. 1 virtual register slot.
        case 'L': {
          ObjPtr<mirror::Object> o = shadow_frame.GetVRegReference(src_reg);
          if (o != nullptr) {
            const dex::TypeIndex type_idx = params->GetTypeItem(shorty_pos).type_idx_;
            ObjPtr<mirror::Class> arg_type = method->GetDexCache()->GetResolvedType(type_idx);
            if (arg_type == nullptr) {
              StackHandleScope<1> hs(self);
              // Preserve o since it is used below and GetClassFromTypeIndex may cause thread
              // suspension.
              HandleWrapperObjPtr<mirror::Object> h = hs.NewHandleWrapper(&o);
              arg_type = method->ResolveClassFromTypeIndex(type_idx);
              if (arg_type == nullptr) {
                CHECK(self->IsExceptionPending());
                return false;
              }
            }
            if (!o->VerifierInstanceOf(arg_type)) {
              // This should never happen.
              std::string temp1, temp2;
              self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                                       "Invoking %s with bad arg %d, type '%s' not instance of '%s'",
                                       WlSafeName(new_shadow_frame->GetMethod()), shorty_pos,
                                       o->GetClass()->GetDescriptor(&temp1),
                                       arg_type->GetDescriptor(&temp2));
              return false;
            }
          }
          new_shadow_frame->SetVRegReference(dest_reg, o);
          break;
        }
        // Handle doubles and longs. 2 consecutive virtual register slots.
        case 'J': case 'D': {
          uint64_t wide_value =
              (static_cast<uint64_t>(shadow_frame.GetVReg(src_reg + 1)) << BitSizeOf<uint32_t>()) |
               static_cast<uint32_t>(shadow_frame.GetVReg(src_reg));
          new_shadow_frame->SetVRegLong(dest_reg, wide_value);
          // Skip the next virtual register slot since we already used it.
          ++dest_reg;
          ++arg_offset;
          break;
        }
        // Handle all other primitives that are always 1 virtual register slot.
        default:
          new_shadow_frame->SetVReg(dest_reg, shadow_frame.GetVReg(src_reg));
          break;
      }
    }
	  } else {
	    if (pfc_diag) {
	      fprintf(stderr,
	              "[PFCUT] DoCallCommon before fast CopyRegisters dexIdx=%u caller_skip=1\n",
	              called_method->GetDexMethodIndex());
      fflush(stderr);
    }
    if (is_range) {
      DCHECK_EQ(num_regs, first_dest_reg + number_of_inputs);
    }

    CopyRegisters<is_range>(shadow_frame,
                            new_shadow_frame,
                            arg,
                            vregC,
                            first_dest_reg,
                            number_of_inputs);
	    self->EndAssertNoThreadSuspension(old_cause);
	  }

  // WESTLAKE §739 (2026-08-20): preserve the proven main-Looper reference at
  // the caller-frame -> callee-frame argument-copy boundary.
  //
  // §738 proved that BasicTikTokFragment's exact stock bytecode dataflow has a
  // live sMainLooper reference in the source vreg immediately before
  // Handler(Looper). §739 then proved that the first constructor frame receives
  // it, but Handler's stock (Looper)->(Looper,Callback,Z)->(...,Z,Z) delegation
  // loses it before the terminal constructor reads mQueue.  Cover each stock
  // Looper-first Handler constructor and repair only a literal copy invariant:
  // a non-null source argument may not become null in the just-built callee
  // frame. Explicit Handler(null) calls remain untouched.
  const std::string wl_handler_signature =
      called_method != nullptr ? called_method->GetSignature().ToString() : std::string();
  const bool wl_handler_looper_ctor =
      called_method != nullptr && !string_init && number_of_inputs >= 2u &&
      PFCutDeclaringClassPlausible(called_method) &&
      called_method->GetDeclaringClass()->DescriptorEquals("Landroid/os/Handler;") &&
      strcmp(WlSafeName(called_method), "<init>") == 0 &&
      (wl_handler_signature == "(Landroid/os/Looper;)V" ||
       wl_handler_signature ==
           "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V" ||
       wl_handler_signature ==
           "(Landroid/os/Looper;Landroid/os/Handler$Callback;Z)V" ||
       wl_handler_signature ==
           "(Landroid/os/Looper;Landroid/os/Handler$Callback;ZZ)V");
  if (wl_handler_looper_ctor) {
    const uint32_t source_reg = is_range ? vregC + 1u : arg[1];
    ObjPtr<mirror::Object> source_looper = shadow_frame.GetVRegReference(source_reg);
    ObjPtr<mirror::Object> copied_before =
        new_shadow_frame->GetVRegReference(first_dest_reg + 1u);
    ObjPtr<mirror::Object> static_main = nullptr;
    const uint32_t current_pc = shadow_frame.GetDexPC();
    const bool exact_flow =
        g_westlake_last_main_looper_non_null &&
        g_westlake_last_main_looper_method != nullptr &&
        g_westlake_last_main_looper_caller == shadow_frame.GetMethod() &&
        current_pc == g_westlake_last_main_looper_dex_pc + 4u;
    if (exact_flow) {
      ObjPtr<mirror::Class> looper_class =
          g_westlake_last_main_looper_method->GetDeclaringClass();
      if (looper_class != nullptr &&
          looper_class->DescriptorEquals("Landroid/os/Looper;")) {
        for (ArtField& field : looper_class->GetSFields()) {
          if (strcmp(field.GetName(), "sMainLooper") == 0) {
            static_main = field.GetObject(looper_class);
            break;
          }
        }
      }
    }
    const bool source_matches_static =
        source_looper != nullptr && source_looper == static_main;
    const bool repaired = source_looper != nullptr && copied_before == nullptr;
    if (repaired) {
      new_shadow_frame->SetVRegReference(first_dest_reg + 1u, source_looper);
    }
    fprintf(stderr,
            "[WESTLAKE-MAINLOOPER-739] caller=%s source=%p copiedBefore=%p "
            "target=%s copiedAfter=%p static=%p exactFlow=%d sourceMatches=%d repaired=%d\n",
            shadow_frame.GetMethod() != nullptr
                ? shadow_frame.GetMethod()->PrettyMethod().c_str() : "<null>",
            source_looper.Ptr(), copied_before.Ptr(),
            wl_handler_signature.c_str(),
            new_shadow_frame->GetVRegReference(first_dest_reg + 1u),
            static_main.Ptr(), exact_flow ? 1 : 0,
            source_matches_static ? 1 : 0, repaired ? 1 : 0);
    fflush(stderr);
    g_westlake_last_main_looper_non_null = false;
  }

	  if (UNLIKELY(called_method->IsProxyMethod())) {
	    if (!EnsureInitialized(self, new_shadow_frame)) {
	      return false;
	    }
	    ArtMethod* interface_method =
	        called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
	    uint32_t shorty_len = 0u;
	    const char* shorty = interface_method->GetShorty(&shorty_len);
	    ObjPtr<mirror::Object> receiver =
	        new_shadow_frame->GetVRegReference(first_dest_reg);
	    if (UNLIKELY(receiver == nullptr)) {
	      ThrowNullPointerException("proxy receiver == null");
	      return false;
	    }

	    ScopedObjectAccessUnchecked soa(self);
	    ScopedJniEnvLocalRefState env_state(soa.Env());
	    jobject receiver_jobj = soa.AddLocalReference<jobject>(receiver);
	    ObjPtr<mirror::Method> interface_reflect_method =
	        mirror::Method::CreateFromArtMethod<kRuntimePointerSize>(soa.Self(), interface_method);
	    if (UNLIKELY(interface_reflect_method == nullptr)) {
	      soa.Self()->AssertPendingOOMException();
	      return false;
	    }
	    jobject interface_method_jobj =
	        soa.AddLocalReference<jobject>(interface_reflect_method);
	    std::vector<jvalue> proxy_args;
	    size_t src_reg = first_dest_reg + 1u;
	    for (uint32_t shorty_pos = 1u; shorty_pos < shorty_len; ++shorty_pos) {
	      jvalue value = {};
	      switch (shorty[shorty_pos]) {
	        case 'L':
	          value.l = soa.AddLocalReference<jobject>(
	              new_shadow_frame->GetVRegReference(src_reg));
	          src_reg++;
	          break;
	        case 'J':
	        case 'D':
	          value.j =
	              (static_cast<uint64_t>(new_shadow_frame->GetVReg(src_reg + 1u))
	               << BitSizeOf<uint32_t>()) |
	              static_cast<uint32_t>(new_shadow_frame->GetVReg(src_reg));
	          src_reg += 2u;
	          break;
	        default:
	          value.j = static_cast<uint32_t>(new_shadow_frame->GetVReg(src_reg));
	          src_reg++;
	          break;
	      }
	      proxy_args.push_back(value);
	    }

	    static thread_local int proxy_interp_count = 0;
	    if (proxy_interp_count < 80) {
	      proxy_interp_count++;
	      fprintf(stderr,
	              "[PFCUT-PROXY] interpreter proxy invoke %s target=%s args=%zu\n",
	              called_method->PrettyMethod().c_str(),
	              interface_method->PrettyMethod().c_str(),
	              proxy_args.size());
	      fflush(stderr);
	    }
	    self->PushShadowFrame(new_shadow_frame);
	    *result = InvokeProxyInvocationHandler(
	        soa, shorty, receiver_jobj, interface_method_jobj, proxy_args);
	    self->PopShadowFrame();
	    return !self->IsExceptionPending();
	  }

	  if (called_method->IsNative()) {
	    if (pfc_diag) {
	      fprintf(stderr,
	              "[PFCUT] DoCallCommon native branch dexIdx=%u new_shadow=%p quick=%p jni=%p\n",
              called_method->GetDexMethodIndex(),
              new_shadow_frame,
              called_method->GetEntryPointFromQuickCompiledCode(),
              called_method->GetEntryPointFromJni());
      fflush(stderr);
    }
    if (!EnsureInitialized(self, new_shadow_frame)) {
      return false;
    }
    // WESTLAKE §699 — trace the two Chromium navigation JNI handoffs without changing them.
    //
    // The Java-side §697 oracle proves AwContents, WebContentsImpl and
    // NavigationControllerImpl all retain live, non-zero native peers.  Toutiao also reaches
    // AwContents.q(), whose only two native operations are Me4$sHFE (attach the serialized
    // load-data payload) and MAqmDh4t (NavigationController::LoadUrl).  Record entry/return and
    // the first native peer argument here so a silent native return can be distinguished from a
    // call which never enters JNI.  This is deliberately diagnostic-only: arguments, result and
    // pending exceptions are left untouched.
    const char* wl_chromium_native_name = nullptr;
    bool wl_chromium_native_trace = false;
    if (PFCutDeclaringClassPlausible(called_method) &&
        called_method->GetDeclaringClass()->DescriptorEquals("LJ/N;")) {
      wl_chromium_native_name = WlSafeName(called_method);
      wl_chromium_native_trace =
          wl_chromium_native_name != nullptr &&
          (strcmp(wl_chromium_native_name, "Me4$sHFE") == 0 ||
           strcmp(wl_chromium_native_name, "MAqmDh4t") == 0);
    }
    if (UNLIKELY(called_method->IsStatic() &&
                 PFCutDeclaringClassPlausible(called_method) &&
                 called_method->GetDeclaringClass()->DescriptorEquals("Ldalvik/system/VMStack;") &&
                 strcmp(WlSafeName(called_method), "getStackClass2") == 0)) {
      ShadowFrame* caller_frame = &shadow_frame;
      for (int i = 0; i < 2 && caller_frame != nullptr; ++i) {
        caller_frame = caller_frame->GetLink();
      }
      ArtMethod* caller_method =
          caller_frame != nullptr ? caller_frame->GetMethod() : nullptr;
      ObjPtr<mirror::Class> caller_class =
          caller_method != nullptr ? caller_method->GetDeclaringClass() : nullptr;
      if (caller_class == nullptr) {
        ScopedStackedShadowFramePusher pusher(self, new_shadow_frame);
        NthCallerVisitor visitor(self, 3);
        visitor.WalkStack();
        caller_method = visitor.caller;
        caller_class = caller_method != nullptr ? caller_method->GetDeclaringClass() : nullptr;
      }
      if (pfc_diag) {
        fprintf(stderr,
                "[PFCUT] DoCallCommon VMStack.getStackClass2 direct caller=%s result=%p\n",
                caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
                caller_class.Ptr());
        fflush(stderr);
      }
      result->SetL(caller_class);
      return true;
    }

    // TRACE: detect caller=String.lastIndexOf(I) calling ANY method
    bool trace_li = false;
    if (WestlakeTraceVerboseCalls() && caller_method && shadow_frame.NumberOfVRegs() <= 4) {
      const char* cname = WlSafeName(caller_method);
      if (cname && strcmp(cname, "lastIndexOf") == 0) {
        int nr = shadow_frame.NumberOfVRegs();
        trace_li = true;
        fprintf(stderr, "[VREG-TRACE] BEFORE %s calling %s (native=%d): ",
                caller_method->PrettyMethod().c_str(),
                called_method->PrettyMethod().c_str(),
                called_method->IsNative());
        for (int i = 0; i < nr; i++) fprintf(stderr, "v%d=%u ", i, shadow_frame.GetVReg(i));
        fprintf(stderr, "\n"); fflush(stderr);
      }
    }

    uint32_t* invoke_args = new_shadow_frame->GetVRegArgs(0);
    uint32_t invoke_size = new_shadow_frame->NumberOfVRegs() * sizeof(uint32_t);
    const char* invoke_shorty = called_method->GetInterfaceMethodIfProxy(
        kRuntimePointerSize)->GetShorty();
    const void* quick_entry = called_method->GetEntryPointFromQuickCompiledCode();
    const void* jni_entry = called_method->GetEntryPointFromJni();
    if (wl_chromium_native_trace) {
      const uint64_t native_peer =
          new_shadow_frame->NumberOfVRegs() >= 2
              ? static_cast<uint64_t>(new_shadow_frame->GetVRegLong(0))
              : 0u;
      fprintf(stderr,
              "[WESTLAKE-WEBVIEW-NATIVE] BEFORE name=%s caller=%s peer=0x%llx "
              "inputs=%u regs=%u quick=%p jni=%p\n",
              wl_chromium_native_name,
              caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
              static_cast<unsigned long long>(native_peer),
              number_of_inputs,
              new_shadow_frame->NumberOfVRegs(),
              quick_entry,
              jni_entry);
      fflush(stderr);
    }
    const bool pfc_stale_native_entry =
        pfc_had_stale_native_quick_entry ||
        PFCutPf625EntryLooksInvalid(quick_entry) ||
        PFCutPf625EntryLooksInvalid(jni_entry);
    bool pfc_force_interpreter_jni = false;
    const char* pfc_force_interpreter_reason = nullptr;
    if (PFCutDeclaringClassPlausible(called_method)) {
      std::string native_desc_storage;
      const char* native_desc =
          called_method->GetDeclaringClass()->GetDescriptor(&native_desc_storage);
      // §319b PROVEN NECESSARY (do NOT remove): Unsafe MUST be forced onto the interpreter-JNI path.
      // Un-forcing (letting the @FastNative quick path run) mismarshalls the `jlong offset` arg — the
      // CAS then hits offset=0 (object header) instead of the real field, always fails, and spins
      // (observed `CAS int offset=0 expected=16 new=-1 success=0` flood, broke init before
      // bindApplication). The slow-but-correct interpreter-JNI path is required. The BouncyCastle
      // slowness (see [[setsurface-sigbus-fixed-launch-spin-2026-07-23]]) must be fixed elsewhere
      // (defer BC in AppSpawnXInit), NOT by touching this.
      pfc_force_interpreter_jni =
          native_desc != nullptr &&
          (strcmp(native_desc, "Lsun/misc/Unsafe;") == 0 ||
           strcmp(native_desc, "Ljdk/internal/misc/Unsafe;") == 0);
      if (pfc_force_interpreter_jni) {
        pfc_force_interpreter_reason = "Unsafe";
      }
      if (!pfc_force_interpreter_jni &&
          native_desc != nullptr &&
          strcmp(native_desc, "Ljava/lang/Class;") == 0 &&
          strcmp(WlSafeName(called_method), "classForName") == 0) {
        pfc_force_interpreter_jni = true;
        pfc_force_interpreter_reason = "Class.classForName";
      }
      if (!pfc_force_interpreter_jni && native_desc != nullptr) {
        std::string thread_name;
        self->GetThreadName(thread_name);
        const bool pfc_default_dispatch =
            thread_name.rfind("DefaultDispatch", 0) == 0;
        if (pfc_default_dispatch &&
            (strcmp(native_desc, "Ljava/lang/String;") == 0 ||
             strcmp(native_desc, "Ljava/lang/System;") == 0)) {
          pfc_force_interpreter_jni = true;
          pfc_force_interpreter_reason = "DefaultDispatch";
          static thread_local int default_dispatch_jni_count = 0;
          if (default_dispatch_jni_count < 80) {
            default_dispatch_jni_count++;
            fprintf(stderr,
                    "[PFCUT] DefaultDispatch interpreter JNI %s quick=%p jni=%p\n",
                    called_method->PrettyMethod().c_str(),
                    quick_entry,
                    jni_entry);
            fflush(stderr);
          }
        }
      }
    }
    const bool use_interpreter_jni =
        pfc_force_interpreter_jni || pfc_stale_native_entry;
    if (pfc_stale_native_entry) {
      PFCutPf625LogUnsafeNativeEntry(
          "docall-native", called_method, caller_method, quick_entry, jni_entry);
      called_method->SetEntryPointFromJniPtrSize(nullptr, kRuntimePointerSize);
    }
    if (use_interpreter_jni && pfc_force_interpreter_jni) {
      static thread_local int forced_jni_count = 0;
      if (forced_jni_count < 0 /*§650*/) {
        forced_jni_count++;
        fprintf(stderr,
                "[PFCUT] forced interpreter JNI reason=%s method=%s quick=%p jni=%p\n",
                pfc_force_interpreter_reason != nullptr ? pfc_force_interpreter_reason : "<unknown>",
                called_method->PrettyMethod().c_str(),
                quick_entry,
                jni_entry);
        fflush(stderr);
      }
    }
    if (use_interpreter_jni) {
      ObjPtr<mirror::Object> receiver = nullptr;
      uint32_t* method_args = invoke_args;
      if (!called_method->IsStatic()) {
        receiver = new_shadow_frame->GetVRegReference(first_dest_reg);
        method_args = new_shadow_frame->GetVRegArgs(first_dest_reg + 1);
      }
      EnterInterpreterFromInvoke(
          self, called_method, receiver, method_args, result, /*stay_in_interpreter=*/ true);
    } else {
      self->PushShadowFrame(new_shadow_frame);
      called_method->Invoke(self, invoke_args, invoke_size, result, invoke_shorty);
      if (pfc_diag) {
        fprintf(stderr,
                "[PFCUT] DoCallCommon after Invoke dexIdx=%u top_shadow=%p top_quick=%p link=%p result=%p\n",
                called_method->GetDexMethodIndex(),
                self->GetManagedStack()->GetTopShadowFrame(),
                self->GetManagedStack()->GetTopQuickFrame(),
                self->GetManagedStack()->GetLink(),
                result != nullptr ? result->GetL() : nullptr);
        fflush(stderr);
      }
      if (pfc_diag) {
        ShadowFrame* top = self->GetManagedStack()->GetTopShadowFrame();
        ShadowFrame* link = top != nullptr ? top->GetLink() : nullptr;
        fprintf(stderr,
                "[PFCUT] DoCallCommon before PopShadowFrame dexIdx=%u top=%p frame=%p link=%p\n",
                called_method->GetDexMethodIndex(),
                top,
                new_shadow_frame,
                link);
        fflush(stderr);
      }
      self->PopShadowFrame();
      if (pfc_diag) {
        ShadowFrame* top = self->GetManagedStack()->GetTopShadowFrame();
        fprintf(stderr,
                "[PFCUT] DoCallCommon after PopShadowFrame dexIdx=%u top=%p top_quick=%p link=%p\n",
                called_method->GetDexMethodIndex(),
                top,
                self->GetManagedStack()->GetTopQuickFrame(),
                self->GetManagedStack()->GetLink());
        fflush(stderr);
      }
    }

    if (wl_chromium_native_trace) {
      const bool returns_object = strcmp(wl_chromium_native_name, "MAqmDh4t") == 0;
      fprintf(stderr,
              "[WESTLAKE-WEBVIEW-NATIVE] AFTER name=%s pending=%d result=%p\n",
              wl_chromium_native_name,
              self->IsExceptionPending() ? 1 : 0,
              returns_object && result != nullptr ? result->GetL() : nullptr);
      fflush(stderr);
    }

    if (trace_li) {
      fprintf(stderr, "[VREG-TRACE] AFTER: v0=%u v1=%u v2=%u\n",
              shadow_frame.GetVReg(0), shadow_frame.GetVReg(1), shadow_frame.GetVReg(2));
      fflush(stderr);
    }
  } else {
    if (pfc_diag) {
      fprintf(stderr,
              "[PFCUT] DoCallCommon managed branch dexIdx=%u new_shadow=%p use_interpreter=%d\n",
              called_method->GetDexMethodIndex(),
              new_shadow_frame,
              use_interpreter_entrypoint ? 1 : 0);
      fflush(stderr);
    }
    // Trace non-native calls.  Do not save/restore raw vreg words here: ShadowFrame
    // keeps reference metadata in a parallel array, and restoring only the words can
    // turn a live moved reference into an untracked primitive.  The upstream ART path
    // calls PerformCall() directly and leaves the caller frame under GC control.
    int nn_nregs = shadow_frame.NumberOfVRegs();

    // Trace ALL calls from methods with <=4 vregs (catches lastIndexOf)
    static thread_local int pc_trace = 0;
    ArtMethod* _cm = shadow_frame.GetMethod();
    const char* _cn = _cm ? WlSafeName(_cm) : nullptr;
    bool do_trace = WestlakeTraceVerboseCalls() &&
        (_cn && strcmp(_cn, "lastIndexOf") == 0) &&
        nn_nregs <= 4 &&
        pc_trace < 8;
    if (do_trace) {
      pc_trace++;
      ArtMethod* cm = shadow_frame.GetMethod();
      fprintf(stderr, "[PC-TRACE] BEFORE %s → %s: ",
              cm ? cm->PrettyMethod().c_str() : "?",
              called_method->PrettyMethod().c_str());
      for (int i = 0; i < nn_nregs; i++) fprintf(stderr, "v%d=0x%x ", i, shadow_frame.GetVReg(i));
      fprintf(stderr, "\n"); fflush(stderr);
    }

    const bool pfc_trace_gson_nnw = PFCutIsGsonNextNonWhitespace(called_method);
    if (pfc_trace_gson_nnw) {
      PFCutLogGsonReaderState("before-PerformCall",
                              called_method,
                              new_shadow_frame,
                              first_dest_reg,
                              result);
    }

    PerformCall(self,
                accessor,
                shadow_frame.GetMethod(),
                first_dest_reg,
                new_shadow_frame,
                result,
                use_interpreter_entrypoint);
    if (pfc_trace_gson_nnw) {
      PFCutLogGsonReaderState("after-PerformCall",
                              called_method,
                              new_shadow_frame,
                              first_dest_reg,
                              result);
    }
    if (pfc_diag) {
      fprintf(stderr,
              "[PFCUT] DoCallCommon after PerformCall dexIdx=%u pending=%d top_shadow=%p top_quick=%p\n",
              called_method->GetDexMethodIndex(),
              self->IsExceptionPending() ? 1 : 0,
              self->GetManagedStack()->GetTopShadowFrame(),
              self->GetManagedStack()->GetTopQuickFrame());
      fflush(stderr);
    }

    if (do_trace) {
      fprintf(stderr, "[PC-TRACE] AFTER: ");
      for (int i = 0; i < nn_nregs; i++) fprintf(stderr, "v%d=0x%x ", i, shadow_frame.GetVReg(i));
      fprintf(stderr, "\n"); fflush(stderr);
    }
  }

  if (string_init && !self->IsExceptionPending()) {
    SetStringInitValueToAllAliases(&shadow_frame, string_init_vreg_this, *result);
  }

  if (self->IsExceptionPending()) {
    ObjPtr<mirror::Object> exception = self->GetException();
    // WESTLAKE §690: Toutiao's article template reaches KtX phase=2 with data ready, but the
    // WebView URL remains empty.  KtR.h() contains no normal return before loadDataWithBaseURL
    // (its sole Robust redirect is live-null), so identify the exact callee whose exception
    // unwinds that method.  Diagnostic only: do not clear or replace the pending exception.
    if (caller_method != nullptr && exception != nullptr && exception->GetClass() != nullptr) {
      const std::string wl_caller = caller_method->PrettyMethod();
      if (wl_caller.find(" X.KtR.h(") != std::string::npos) {
        static thread_local int wl_kt_r_throw_count = 0;
        if (wl_kt_r_throw_count < 40) {
          ++wl_kt_r_throw_count;
          std::string wl_message_storage;
          const char* wl_message = PFCutThrowableMessage(exception, &wl_message_storage);
          fprintf(stderr,
                  "[WESTLAKE-KTR-THROW] caller=%s callee=%s dex_pc=%u exception=%s message=%s\n",
                  wl_caller.c_str(),
                  called_method != nullptr ? called_method->PrettyMethod().c_str() : "<null>",
                  shadow_frame.GetDexPC(),
                  exception->GetClass()->PrettyDescriptor().c_str(),
                  wl_message != nullptr ? wl_message : "<null>");
          fflush(stderr);
        }
      }
    }
    if (exception != nullptr &&
        exception->GetClass() != nullptr &&
        exception->GetClass()->DescriptorEquals("Ljava/lang/UnsupportedOperationException;")) {
      static thread_local int pfc_uoe_return_count = 0;
      if (pfc_uoe_return_count < 80) {
        pfc_uoe_return_count++;
        std::string message_storage;
        const char* message = PFCutThrowableMessage(exception, &message_storage);
        fprintf(stderr,
                "[PFCUT] pending UOE return caller=%s callee=%s inputs=%u native=%d code=%p "
                "message=%s\n",
                caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
                called_method != nullptr ? called_method->PrettyMethod().c_str() : "<null>",
                number_of_inputs,
                called_method != nullptr && called_method->IsNative() ? 1 : 0,
                called_method != nullptr ? called_method->GetCodeItem() : nullptr,
                message);
        fflush(stderr);
      }
    }
    if (exception != nullptr &&
        exception->GetClass() != nullptr &&
        exception->GetClass()->DescriptorEquals("Ljava/lang/NullPointerException;")) {
      std::string message_storage;
      const char* message = PFCutThrowableMessage(exception, &message_storage);
      if (message != nullptr && strcmp(message, "charset") == 0) {
        static thread_local int pfc_charset_npe_count = 0;
        if (pfc_charset_npe_count < 80) {
          pfc_charset_npe_count++;
          fprintf(stderr,
                  "[PFCUT-NPE-CHARSET] caller=%s callee=%s inputs=%u native=%d code=%p\n",
                  caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
                  called_method != nullptr ? called_method->PrettyMethod().c_str() : "<null>",
                  number_of_inputs,
                  called_method != nullptr && called_method->IsNative() ? 1 : 0,
                  called_method != nullptr ? called_method->GetCodeItem() : nullptr);
          fflush(stderr);
        }
      }
    }
  }

  return !self->IsExceptionPending();
}

bool g_westlake_infl_gate = false;  // WESTLAKE §212d (read by Thread::SetException)

template<bool is_range>
NO_STACK_PROTECTOR
bool DoCall(ArtMethod* called_method,
            Thread* self,
            ShadowFrame& shadow_frame,
            const Instruction* inst,
            uint16_t inst_data,
            bool is_string_init,
            JValue* result) {
  ArtMethod* caller_method = shadow_frame.GetMethod();
  // WESTLAKE 2026-07-22: the app SIGSEGVs in DoCall with an ArtMethod* whose value is ASCII
  // ("on/icon", fault 0x006e6f63692f6e6f). The faultlog is 96 frames of interpreter internals and
  // names no Java method, and a probe in DoCallCommon never fired (DoCall faults BEFORE reaching
  // it). Validate the POINTER VALUE here -- never dereference a structurally invalid method.
  // Printable address bytes are not evidence of corruption; see the matching DoCallCommon guard.
  {
    const uintptr_t m = reinterpret_cast<uintptr_t>(called_method);
    const bool insane = (m & 0x3) != 0 || (m >> 48) != 0;
    if (m == 0 || insane) {
      char txt[9];
      for (int i = 0; i < 8; ++i) {
        const unsigned b = static_cast<unsigned>((m >> (i * 8)) & 0xff);
        txt[i] = (b >= 0x20 && b <= 0x7e) ? static_cast<char>(b) : '.';
      }
      txt[8] = '\0';
      fprintf(stderr,
              "[WESTLAKE-BADCALL] corrupt called_method=%p ascii=\"%s\" caller=%s dex_pc=%u "
              "inst_data=0x%04x string_init=%d\n",
              called_method, txt,
              caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
              shadow_frame.GetDexPC(), inst_data, is_string_init ? 1 : 0);
      fflush(stderr);
    }
  }
  // WESTLAKE §669 (2026-08-16): ENSURE THE DECLARING CLASS IS INITIALISED before a static call.
  // Measured (§668): `java.lang.invoke.VarHandle.<clinit>` NEVER runs — it appears neither on
  // InitializeClass's success path nor its tolerate path, in the zygote or the child — yet
  // `VarHandle.acquireFence()` executes and reads `VarHandle.UNSAFE` as NULL, so
  // `sun.misc.Unsafe.loadFence()` is invoked on null. (`sun.misc.Unsafe.<clinit>` DOES run and sets
  // THE_ONE=0x14025100, so the value it should have had was available.)
  // In stock ART, resolving a static invoke initialises the declaring class; on this port the
  // interpreter-cache / §440-§551 repair paths can hand back an ArtMethod without that ever
  // happening. That breaks EVERY class whose statics are set up in <clinit>, which is why it
  // surfaced in libcore (ConcurrentSkipListMap -> VarHandle fences), not in app code.
  // Cheap: IsInitialized() is a field read, and the slow path runs at most once per class.
  if (called_method != nullptr && called_method->IsStatic()) {
    ObjPtr<mirror::Class> wl_decl = called_method->GetDeclaringClass();
    // §669b: is this path even reached, and what does the class look like when it is?
    if (UNLIKELY(wl_decl != nullptr) && access("/data/local/tmp/asx/CINIT", F_OK) == 0) {
      std::string wl_t;
      const char* wl_d = wl_decl->GetDescriptor(&wl_t);
      if (wl_d != nullptr && strcmp(wl_d, "Ljava/lang/invoke/VarHandle;") == 0) {
        static thread_local int wl_vh = 0;
        if (wl_vh < 6) {
          wl_vh++;
          fprintf(stderr, "[WESTLAKE-669b] static invoke %s status=%d initialized=%d\n",
                  WlSafeName(called_method), static_cast<int>(wl_decl->GetStatus()),
                  wl_decl->IsInitialized() ? 1 : 0);
          fflush(stderr);
        }
      }
    }
    if (UNLIKELY(wl_decl != nullptr && !wl_decl->IsInitialized())) {
      StackHandleScope<1> wl_hs(self);
      Handle<mirror::Class> wl_h(wl_hs.NewHandle(wl_decl));
      ClassLinker* wl_cl = Runtime::Current()->GetClassLinker();
      if (!wl_cl->EnsureInitialized(self, wl_h, true, true)) {
        static thread_local int wl_ei = 0;
        if (wl_ei < 20 && access("/data/local/tmp/asx/CINIT", F_OK) == 0) {
          wl_ei++;
          fprintf(stderr, "[WESTLAKE-669] EnsureInitialized FAILED for %s (static invoke)\n",
                  called_method->PrettyMethod().c_str());
          fflush(stderr);
        }
        if (self->IsExceptionPending()) {
          return false;   // propagate, exactly as stock resolution would
        }
      }
    }
  }

  // WESTLAKE §217b: the §211-§213 inflation probe lived here and has been REMOVED.
  // It decoded the invoke's receiver vreg and called wl_o->GetClass()->PrettyDescriptor(); a vreg
  // that is not a live reference made that fault -- SIGSEGV in DoCall at `ldr w0,[x8]` right before
  // `bl mirror::Class::PrettyDescriptor` (tombstone cppcrash-1317, addr 0x0a6a2d59). It killed a
  // thread that held a lock, wedging the main thread in futex_wait_queue_me, so runs stalled early.
  // The probe found §216; do NOT reinstate it without validating the ObjPtr the way §206 does.
  // WESTLAKE §218: trace the activity-resume / window-attach path. METHOD-ONLY -- logs only
  // called_method (already validated below/above) and NEVER decodes a receiver vreg, which is what
  // made the §211d probe crash the app (§217).
  {
    static pid_t wl_wpid = 0;
    static int wl_wn = 0;
    if (wl_wpid != getpid()) { wl_wpid = getpid(); wl_wn = 0; }
    const uintptr_t wl_wm = reinterpret_cast<uintptr_t>(called_method);
    // WESTLAKE §282b: the cap used to be 400 and the filter included isEnabled/getVisibility.
    // AccessibilityManager.isEnabled ALONE burned 322 of the 400 slots, so the trace stopped
    // long before the interesting calls and `draw` read as ZERO occurrences even when it ran.
    // That measurement artifact is exactly the "prove the probe is reachable before trusting
    // a 0" trap.  Drop the high-frequency no-signal names and raise the cap.
    if (wl_wn < 4000 && wl_wm > 0x10000 && (wl_wm & 3u) == 0 && (wl_wm >> 48) == 0 &&
        WlNameReadable(called_method)) {   // §635: this is where Toutiao SIGSEGVs (DoCall+0x38c)
      const char* wl_wd = WlSafeDescriptor(called_method);
      const char* wl_wnm = WlSafeName(called_method);
      if (wl_wd != nullptr && wl_wnm != nullptr &&
          (strstr(wl_wd, "SurfaceControl") != nullptr ||
           strcmp(wl_wnm, "copyFrom") == 0 ||
           strcmp(wl_wnm, "transferFrom") == 0 ||
           strcmp(wl_wnm, "syncAndDrawFrame") == 0 ||
           strcmp(wl_wnm, "nSyncAndDrawFrame") == 0 ||
           strcmp(wl_wnm, "draw") == 0 ||
           strcmp(wl_wnm, "drawSoftware") == 0 ||
           strcmp(wl_wnm, "performDraw") == 0 ||
           strcmp(wl_wnm, "performTraversals") == 0 ||
           strcmp(wl_wnm, "relayoutWindow") == 0 ||
           strcmp(wl_wnm, "updateDisplayState") == 0 ||
           strcmp(wl_wnm, "getOrCreateBLASTSurface") == 0 ||
           // WESTLAKE §282d: static analysis of framework.jar shows EXACTLY two writers of
           // ViewRootImpl.mSurface -- relayoutWindow() -> Surface.copyFrom(SurfaceControl)
           // and updateBlastSurfaceIfNeeded() -> Surface.transferFrom(Surface).  relayoutWindow
           // runs 24x and mSurfaceControl.mNativeObject is a VALID 0x7f2b9fba30, yet neither
           // writer ever runs and mSurface.mNativeObject stays 0x0.  Trace the BLAST path to
           // find which guard swallows it.
           // WESTLAKE §283l: name the hwui entry point that blocks the UI thread.  With a real
           // session mSurface goes VALID and then the UI thread deadlocks on a futex while
           // RenderThread sits idle in epoll_wait => a RenderProxy runSync() hand-off that is
           // never delivered/signalled.  These are the Java-side doors into RenderProxy.
           strcmp(wl_wnm, "setSurface") == 0 ||
           strcmp(wl_wnm, "nSetSurface") == 0 ||
           strcmp(wl_wnm, "initialize") == 0 ||
           strcmp(wl_wnm, "nInitialize") == 0 ||
           strcmp(wl_wnm, "nCreateProxy") == 0 ||
           strcmp(wl_wnm, "nSetLightGeometry") == 0 ||
           strcmp(wl_wnm, "nSetLightAlpha") == 0 ||
           strcmp(wl_wnm, "nSetName") == 0 ||
           strcmp(wl_wnm, "nSetSurfaceControl") == 0 ||
           strcmp(wl_wnm, "updateBlastSurfaceIfNeeded") == 0 ||
           strcmp(wl_wnm, "isSameSurfaceControl") == 0 ||
           strcmp(wl_wnm, "useBLAST") == 0 ||
           strcmp(wl_wnm, "createSurface") == 0 ||
           strcmp(wl_wnm, "isValid") == 0 ||
           strcmp(wl_wnm, "handleResumeActivity") == 0 ||
           strcmp(wl_wnm, "performResume") == 0 ||
           strcmp(wl_wnm, "makeVisible") == 0 ||
           strcmp(wl_wnm, "setView") == 0)) {
        wl_wn++;
        // WESTLAKE §250: identity probe. §249 ruled out ordering, so we need to know whether the
        // View made VISIBLE is the same object ViewRootImpl holds as mView.
        // ★Validate the ObjPtr before dereferencing (§206/§217): a probe that dereferences a raw
        // vreg crashed the very run it was measuring.
        std::string wl_extra;
        {
          uint32_t wl_rv;
          if (is_range) {
            wl_rv = inst->VRegC_3rc();
          } else {
            uint32_t wl_a[Instruction::kMaxVarArgRegs];
            inst->GetVarArgs(wl_a, inst_data);
            wl_rv = wl_a[0];
          }
          ObjPtr<mirror::Object> wl_o = shadow_frame.GetVRegReference(wl_rv);
          const uintptr_t wl_op = reinterpret_cast<uintptr_t>(wl_o.Ptr());
          if (wl_op > 0x10000u && (wl_op & 3u) == 0u && (wl_op >> 48) == 0) {
            char wl_buf[224];
            if (strcmp(wl_wnm, "performTraversals") == 0) {
              // receiver is the ViewRootImpl -- read its mView field
              ObjPtr<mirror::Object> wl_mv = nullptr;
              for (ObjPtr<mirror::Class> k = wl_o->GetClass(); k != nullptr; k = k->GetSuperClass()) {
                for (ArtField& f : k->GetIFields()) {
                  if (strcmp(f.GetName(), "mView") == 0) { wl_mv = f.GetObject(wl_o); break; }
                }
                if (wl_mv != nullptr) { break; }
              }
              snprintf(wl_buf, sizeof(wl_buf), " vri=%p mView=%p",
                       reinterpret_cast<void*>(wl_o.Ptr()), reinterpret_cast<void*>(wl_mv.Ptr()));
            } else if (strcmp(wl_wnm, "isValid") == 0) {
              // WESTLAKE §282c: Surface.isValid()/SurfaceControl.isValid() are both just
              // `mNativeObject != 0`.  ViewRootImpl.draw() runs (22x) but never reaches
              // syncAndDrawFrame/drawSoftware, and Surface.copyFrom/transferFrom NEVER run,
              // so the early return at `!mSurface.isValid()` is the prime suspect.  Logging
              // the CALL tells us nothing -- log the FIELD the call reads.
              int64_t wl_np = -1;
              for (ObjPtr<mirror::Class> k = wl_o->GetClass(); k != nullptr; k = k->GetSuperClass()) {
                bool wl_found = false;
                for (ArtField& f : k->GetIFields()) {
                  if (strcmp(f.GetName(), "mNativeObject") == 0) {
                    wl_np = f.GetLong(wl_o);
                    wl_found = true;
                    break;
                  }
                }
                if (wl_found) { break; }
              }
              snprintf(wl_buf, sizeof(wl_buf), " recv=%p cls=%s mNativeObject=0x%llx",
                       reinterpret_cast<void*>(wl_o.Ptr()),
                       wl_o->GetClass()->PrettyDescriptor().c_str(),
                       static_cast<unsigned long long>(wl_np));
            } else if (strcmp(wl_wnm, "setVisibility") == 0) {
              // §251: the ARGUMENT decides everything (VISIBLE=0, INVISIBLE=4, GONE=8).
              // arg[1] is an int vreg -- read it directly, no ObjPtr involved.
              uint32_t wl_vis = 0xffffffffu;
              if (!is_range) {
                uint32_t wl_a2[Instruction::kMaxVarArgRegs];
                inst->GetVarArgs(wl_a2, inst_data);
                wl_vis = shadow_frame.GetVReg(wl_a2[1]);
              } else {
                wl_vis = shadow_frame.GetVReg(inst->VRegC_3rc() + 1);
              }
              snprintf(wl_buf, sizeof(wl_buf), " recv=%p vis=%d cls=%s",
                       reinterpret_cast<void*>(wl_o.Ptr()), (int)wl_vis,
                       wl_o->GetClass()->PrettyDescriptor().c_str());
            } else {
              snprintf(wl_buf, sizeof(wl_buf), " recv=%p cls=%s",
                       reinterpret_cast<void*>(wl_o.Ptr()),
                       wl_o->GetClass()->PrettyDescriptor().c_str());
            }
            wl_extra = wl_buf;
          }
        }
        fprintf(stderr, "[WESTLAKE-WIN] %s.%s%s\n", wl_wd, wl_wnm, wl_extra.c_str());
        fflush(stderr);
      }
    }
  }
  // WESTLAKE 2026-07-22 (§109): called_method proved VALID at both DoCall and DoCallCommon entry,
  // so the ASCII "on/icon" pointer is an OBJECT REFERENCE, not the method. Scan this frame's
  // reference vregs for a pointer whose bytes are printable ASCII -- that is the corrupt receiver
  // /argument DoCall is about to dereference. Prints only on detection, so it is free otherwise.
  // WESTLAKE §313: this §109 BADREF scan ran on EVERY interpreter DoCallCommon (a per-call vreg
  // scan + fprintf) and fired 33,241 times/run, pinning the app main thread at ~100% CPU so it
  // never finished app-init before the AMS 16s kill -> the RenderThread was starved and noice never
  // rendered. It is a DIAGNOSTIC (the "corrupt receiver" bug it hunted was closed long ago), so gate
  // it OFF by default behind WL_BADREF; the per-call cost is now a single cached bool check.
  static const bool wl_badref_on = (getenv("WL_BADREF") != nullptr);
  if (__builtin_expect(wl_badref_on, 0)) {
    const uint32_t nvregs = shadow_frame.NumberOfVRegs();
    for (uint32_t i = 0; i < nvregs; ++i) {
      mirror::Object* o = shadow_frame.GetVRegReference(i);
      const uintptr_t p = reinterpret_cast<uintptr_t>(o);
      if (p == 0) continue;
      bool ascii = true;
      for (int k = 0; k < 8; ++k) {
        const unsigned b = static_cast<unsigned>((p >> (k * 8)) & 0xff);
        if (b != 0 && (b < 0x20 || b > 0x7e)) { ascii = false; break; }
      }
      if (ascii || (p & 0x3) != 0 || (p >> 48) != 0) {
        char t2[9];
        for (int k = 0; k < 8; ++k) {
          const unsigned b = static_cast<unsigned>((p >> (k * 8)) & 0xff);
          t2[k] = (b >= 0x20 && b <= 0x7e) ? static_cast<char>(b) : '.';
        }
        t2[8] = '\0';
        fprintf(stderr,
                "[WESTLAKE-BADREF] vreg%u=%p ascii=\"%s\" caller=%s dex_pc=%u callee=%s\n",
                i, o, t2,
                caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
                shadow_frame.GetDexPC(),
                called_method != nullptr ? called_method->PrettyMethod().c_str() : "<null>");
        fflush(stderr);
        break;   // one report per DoCall is enough
      }
    }
  }
  const bool pfc_l3_trace = PFCutIsL3OnCreate(caller_method);
  const bool pfc_diag = false;
  UNUSED(pfc_l3_trace);
  if (pfc_diag) {
    fprintf(stderr,
            "[PFCUT] DoCall enter dexIdx=%u called_method=%p inst_data=0x%04x caller=%p top_shadow=%p top_quick=%p\n",
            called_method->GetDexMethodIndex(),
            called_method,
            inst_data,
            shadow_frame.GetMethod(),
            self->GetManagedStack()->GetTopShadowFrame(),
            self->GetManagedStack()->GetTopQuickFrame());
    fflush(stderr);
  }
  const bool pfc_last_index_trace = false;
  if (UNLIKELY(pfc_last_index_trace)) {
    static thread_local int dc_trace = 0;
    ArtMethod* cm = shadow_frame.GetMethod();
    if (cm && dc_trace < 5) {
      const char* cn = WlSafeName(cm);
      if (cn && strcmp(cn, "lastIndexOf") == 0) {
        dc_trace++;
        int nr = shadow_frame.NumberOfVRegs();
        fprintf(stderr, "[DoCall-TRACE] %s -> %s: ", cm->PrettyMethod().c_str(),
                called_method->PrettyMethod().c_str());
        for (int i = 0; i < nr && i < 4; i++) fprintf(stderr, "v%d=0x%x ", i, shadow_frame.GetVReg(i));
        fprintf(stderr, "\n"); fflush(stderr);
      }
    }
  }
  // Argument word count.
  const uint16_t number_of_inputs =
      (is_range) ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);

  {
    static thread_local int mcd_oncreate_trace = 0;
    static thread_local bool trace_real_mcd_path = false;
    ArtMethod* caller = shadow_frame.GetMethod();
    const bool trace_mcd_calls = WestlakeTraceMcdCalls();
    const int mcd_trace_limit = trace_mcd_calls ? 80000 : 200;
    if (caller != nullptr && called_method != nullptr && mcd_oncreate_trace < mcd_trace_limit) {
      std::string caller_desc_storage;
      std::string called_desc_storage;
      const char* caller_desc = PFCutDeclaringClassPlausible(caller)
          ? caller->GetDeclaringClass()->GetDescriptor(&caller_desc_storage)
          : "";
      const char* called_desc = PFCutDeclaringClassPlausible(called_method)
          ? called_method->GetDeclaringClass()->GetDescriptor(&called_desc_storage)
          : "";
      const bool is_mcd_frame =
          (caller_desc != nullptr && strstr(caller_desc, "mcdonalds") != nullptr) ||
          (called_desc != nullptr && strstr(called_desc, "mcdonalds") != nullptr);
      const bool is_mcd_splash_lifecycle =
          (caller_desc != nullptr &&
              (strstr(caller_desc, "Lcom/mcdonalds/mcdcoreapp/common/activity/SplashActivity") != nullptr ||
               strstr(caller_desc, "Lcom/mcdonalds/mcdcoreapp/common/activity/Hilt_SplashActivity") != nullptr ||
               strstr(caller_desc, "Lcom/mcdonalds/mcdcoreapp/common/activity/McdLauncherActivity") != nullptr)) ||
          (called_desc != nullptr &&
              (strstr(called_desc, "Lcom/mcdonalds/mcdcoreapp/common/activity/SplashActivity") != nullptr ||
               strstr(called_desc, "Lcom/mcdonalds/mcdcoreapp/common/activity/Hilt_SplashActivity") != nullptr ||
               strstr(called_desc, "Lcom/mcdonalds/mcdcoreapp/common/activity/McdLauncherActivity") != nullptr));
      const bool is_startup_library =
          (caller_desc != nullptr &&
              (strstr(caller_desc, "newrelic") != nullptr ||
               strstr(caller_desc, "androidx/appcompat") != nullptr ||
               strstr(caller_desc, "androidx/core/splashscreen") != nullptr ||
               strstr(caller_desc, "androidx/activity") != nullptr ||
               strstr(caller_desc, "androidx/fragment") != nullptr ||
               strstr(caller_desc, "androidx/lifecycle") != nullptr ||
               strstr(caller_desc, "androidx/savedstate") != nullptr ||
               strstr(caller_desc, "androidx/core/app") != nullptr ||
               strstr(caller_desc, "androidx/core/view") != nullptr ||
               strstr(caller_desc, "androidx/loader") != nullptr ||
               strstr(caller_desc, "com/google/gson") != nullptr ||
               strstr(caller_desc, "java/lang/reflect") != nullptr ||
               strstr(caller_desc, "kotlin/jvm/internal") != nullptr ||
               strstr(caller_desc, "kotlin/Lazy") != nullptr)) ||
          (called_desc != nullptr &&
              (strstr(called_desc, "newrelic") != nullptr ||
               strstr(called_desc, "androidx/appcompat") != nullptr ||
               strstr(called_desc, "androidx/core/splashscreen") != nullptr ||
               strstr(called_desc, "androidx/activity") != nullptr ||
               strstr(called_desc, "androidx/fragment") != nullptr ||
               strstr(called_desc, "androidx/lifecycle") != nullptr ||
               strstr(called_desc, "androidx/savedstate") != nullptr ||
               strstr(called_desc, "androidx/core/app") != nullptr ||
               strstr(called_desc, "androidx/core/view") != nullptr ||
               strstr(called_desc, "androidx/loader") != nullptr ||
               strstr(called_desc, "com/google/gson") != nullptr ||
               strstr(called_desc, "java/lang/reflect") != nullptr ||
               strstr(called_desc, "kotlin/jvm/internal") != nullptr ||
               strstr(called_desc, "kotlin/Lazy") != nullptr));
      if (is_mcd_frame || is_startup_library) {
        trace_real_mcd_path = true;
      }
      const bool boundary_signal =
          is_mcd_splash_lifecycle ||
          (called_method->IsNative() && is_mcd_frame);
      const bool broad_signal =
          is_mcd_frame ||
          is_startup_library ||
          (trace_real_mcd_path && called_method->IsNative());
      if ((trace_mcd_calls && broad_signal) || (!trace_mcd_calls && boundary_signal)) {
        mcd_oncreate_trace++;
        fprintf(stderr,
                "[MCD-CALL] %s -> %s inputs=%u quick=%p jni=%p native=%d code=%p splash=%d\n",
                caller->PrettyMethod().c_str(),
                called_method->PrettyMethod().c_str(),
                number_of_inputs,
                called_method->GetEntryPointFromQuickCompiledCode(),
                called_method->IsNative() ? called_method->GetEntryPointFromJni() : nullptr,
                called_method->IsNative() ? 1 : 0,
                called_method->GetCodeItem(),
                is_mcd_splash_lifecycle ? 1 : 0);
        fflush(stderr);
      }
    }
  }

  // TODO: find a cleaner way to separate non-range and range information without duplicating
  //       code.
  uint32_t arg[Instruction::kMaxVarArgRegs] = {};  // only used in invoke-XXX.
  uint32_t vregC = 0;
  if (is_range) {
    vregC = inst->VRegC_3rc();
  } else {
    vregC = inst->VRegC_35c();
    inst->GetVarArgs(arg, inst_data);
  }

  if (UNLIKELY(called_method != nullptr &&
               called_method->IsNative() &&
               PFCutDeclaringClassPlausible(called_method) &&
               called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/Character;"))) {
    auto get_arg = [&](uint32_t index) -> int32_t {
      const uint32_t source_vreg = is_range ? vregC + index : arg[index];
      return static_cast<int32_t>(shadow_frame.GetVReg(source_vreg));
    };
    const std::string_view method_name = called_method->GetNameView();
    const int32_t cp = number_of_inputs > 0 ? get_arg(0) : 0;
    bool handled = true;
    if (method_name == "isDigitImpl") {
      result->SetZ(cp >= '0' && cp <= '9');
    } else if (method_name == "isLetterImpl") {
      result->SetZ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'));
    } else if (method_name == "isLetterOrDigitImpl") {
      result->SetZ((cp >= '0' && cp <= '9') ||
                   (cp >= 'A' && cp <= 'Z') ||
                   (cp >= 'a' && cp <= 'z'));
    } else if (method_name == "isUpperCaseImpl") {
      result->SetZ(cp >= 'A' && cp <= 'Z');
    } else if (method_name == "isLowerCaseImpl") {
      result->SetZ(cp >= 'a' && cp <= 'z');
    } else if (method_name == "isWhitespaceImpl" || method_name == "isSpaceCharImpl") {
      result->SetZ(cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f');
    } else if (method_name == "toUpperCaseImpl") {
      result->SetI((cp >= 'a' && cp <= 'z') ? cp - ('a' - 'A') : cp);
    } else if (method_name == "toLowerCaseImpl") {
      result->SetI((cp >= 'A' && cp <= 'Z') ? cp + ('a' - 'A') : cp);
    } else if (method_name == "digitImpl") {
      const int32_t radix = number_of_inputs > 1 ? get_arg(1) : 10;
      int32_t value = -1;
      if (cp >= '0' && cp <= '9') {
        value = cp - '0';
      } else if (cp >= 'A' && cp <= 'Z') {
        value = cp - 'A' + 10;
      } else if (cp >= 'a' && cp <= 'z') {
        value = cp - 'a' + 10;
      }
      result->SetI((value >= 0 && value < radix) ? value : -1);
    } else if (method_name == "getTypeImpl") {
      int32_t type = 0;
      if (cp >= '0' && cp <= '9') {
        type = 9;   // Character.DECIMAL_DIGIT_NUMBER
      } else if (cp >= 'A' && cp <= 'Z') {
        type = 1;   // Character.UPPERCASE_LETTER
      } else if (cp >= 'a' && cp <= 'z') {
        type = 2;   // Character.LOWERCASE_LETTER
      } else if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') {
        type = 12;  // Character.SPACE_SEPARATOR
      }
      result->SetI(type);
    } else {
      handled = false;
    }
    if (handled) {
      static uint32_t log_count = 0;
      if (log_count < 32) {
        fprintf(stderr,
                "[PFCUT-CHAR] handled java.lang.Character.%.*s cp=%d inputs=%u\n",
                static_cast<int>(method_name.size()),
                method_name.data(),
                cp,
                number_of_inputs);
        fflush(stderr);
        ++log_count;
      }
      return true;
    }
  }

  // WESTLAKE §738 (2026-08-19): repair the one-instruction managed-result
  // handoff only when its bytecode provenance proves the intended value.
  //
  // §737 measured getMainLooper() returning the live sMainLooper object, while
  // the immediately following Handler(Looper) constructor still received
  // null.  That narrows the corruption to move-result-object / caller-vreg
  // transfer.  Do not turn every null Handler argument into the main Looper:
  // Android deliberately throws for explicit null.  Require all of:
  //   * this is precisely Handler(Looper),
  //   * the argument vreg is null,
  //   * the same thread just invoked getMainLooper from this same caller,
  //   * this invoke is exactly four code units later (3-unit invoke-static,
  //     then 1-unit move-result-object), and
  //   * sMainLooper remains non-null.
  // That is the stock bytecode dataflow Toutiao emitted, expressed generically
  // for any app whose interpreter loses this particular managed result.
  if (called_method != nullptr && !is_string_init && number_of_inputs == 2u &&
      PFCutDeclaringClassPlausible(called_method) &&
      called_method->GetDeclaringClass()->DescriptorEquals("Landroid/os/Handler;") &&
      strcmp(WlSafeName(called_method), "<init>") == 0 &&
      called_method->GetSignature().ToString() == "(Landroid/os/Looper;)V") {
    const uint32_t looper_reg = is_range ? vregC + 1u : arg[1];
    ObjPtr<mirror::Object> looper_arg = shadow_frame.GetVRegReference(looper_reg);
    ObjPtr<mirror::Object> static_main = nullptr;
    const uint32_t current_pc = shadow_frame.GetDexPC();
    const bool exact_flow =
        g_westlake_last_main_looper_non_null &&
        g_westlake_last_main_looper_method != nullptr &&
        g_westlake_last_main_looper_caller == caller_method &&
        current_pc == g_westlake_last_main_looper_dex_pc + 4u;
    if (looper_arg == nullptr && exact_flow) {
      ObjPtr<mirror::Class> looper_class =
          g_westlake_last_main_looper_method->GetDeclaringClass();
      if (looper_class != nullptr &&
          looper_class->DescriptorEquals("Landroid/os/Looper;")) {
        for (ArtField& field : looper_class->GetSFields()) {
          if (strcmp(field.GetName(), "sMainLooper") == 0) {
            static_main = field.GetObject(looper_class);
            break;
          }
        }
      }
      if (static_main != nullptr) {
        shadow_frame.SetVRegReference(looper_reg, static_main);
      }
    }
    const bool repaired = looper_arg == nullptr && exact_flow && static_main != nullptr;
    fprintf(stderr,
            "[WESTLAKE-MAINLOOPER-738] caller=%s arg=%p lastPc=%u currentPc=%u "
            "exactFlow=%d static=%p repaired=%d\n",
            caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
            looper_arg.Ptr(), g_westlake_last_main_looper_dex_pc, current_pc,
            exact_flow ? 1 : 0, static_main.Ptr(), repaired ? 1 : 0);
    fflush(stderr);
    // DoCallCommon consumes this provenance after it has inspected the copied
    // callee argument (§739).  Clearing it here would hide the copy mismatch.
  }

  if (pfc_diag) {
    fprintf(stderr,
            "[PFCUT] DoCall before DoCallCommon dexIdx=%u inputs=%u vregC=%u arg0=%u arg1=%u arg2=%u arg3=%u arg4=%u\n",
            called_method->GetDexMethodIndex(),
            number_of_inputs,
            vregC,
            arg[0],
            arg[1],
            arg[2],
            arg[3],
            arg[4]);
    fflush(stderr);
  }

  {
    ArtMethod* caller = shadow_frame.GetMethod();
    if (LIKELY(called_method != nullptr) && LIKELY(caller != nullptr) && !is_string_init) {
      const Instruction::Code opcode = inst->Opcode(inst_data);
      InvokeType invoke_type = kVirtual;
      switch (opcode) {
        case Instruction::INVOKE_DIRECT:
        case Instruction::INVOKE_DIRECT_RANGE:
          invoke_type = kDirect;
          break;
        case Instruction::INVOKE_INTERFACE:
        case Instruction::INVOKE_INTERFACE_RANGE:
          invoke_type = kInterface;
          break;
        case Instruction::INVOKE_STATIC:
        case Instruction::INVOKE_STATIC_RANGE:
          invoke_type = kStatic;
          break;
        case Instruction::INVOKE_SUPER:
        case Instruction::INVOKE_SUPER_RANGE:
          invoke_type = kSuper;
          break;
        case Instruction::INVOKE_VIRTUAL:
        case Instruction::INVOKE_VIRTUAL_RANGE:
          invoke_type = kVirtual;
          break;
        default:
          break;
      }
      const uint32_t method_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
      const DexFile* dex_file = caller->GetDexFile();
      const dex::MethodId& method_id = dex_file->GetMethodId(method_idx);
      const std::string_view expected_name = dex_file->GetStringView(method_id.name_idx_);
      const Signature expected_signature = dex_file->GetMethodSignature(method_id);
      // WESTLAKE 2026-07-22 (§140 TEST): FindVirtualMethodForInterface REQUIRES that the receiver
      // class implements called_method's declaring interface; that precondition is only a DCHECK,
      // compiled out under -DNDEBUG. If it does not hold, it indexes an IfTable method array with a
      // foreign method index and returns garbage -> the SIGSEGV. Gate to A/B without a rebuild.
      if ((getenv("WESTLAKE_NO_PROXYFIX") == nullptr) &&
          invoke_type == kInterface && !called_method->IsProxyMethod()) {
        const uint32_t receiver_reg = is_range ? vregC : arg[0];
        ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(receiver_reg);
        // WESTLAKE 2026-07-22 (§140 FIX): FindVirtualMethodForInterface REQUIRES that the receiver
        // class implements called_method's declaring interface -- it indexes
        // GetMethodArray(i)->Get(called_method->GetMethodIndex()) after matching the interface.
        // That precondition is only a DCHECK and is compiled out under -DNDEBUG, so when it does
        // not hold the walk returns unrelated heap bytes and the caller dereferences them (the
        // long-standing SIGSEGV at the ASCII address "on/icon"). Verify it explicitly.
        ObjPtr<mirror::Class> wl_iface =
            (called_method != nullptr) ? called_method->GetDeclaringClass() : nullptr;
        const bool wl_ok = (receiver != nullptr) && (wl_iface != nullptr) &&
                           wl_iface->IsInterface() && receiver->GetClass()->Implements(wl_iface);
        if (wl_ok) {
          // §673: bounds-checked -- upstream's version returns adjacent heap bytes when
          // called_method's method_index_ is past the end of the receiver's method array.
          ArtMethod* proxy_replacement = WlFindVirtualMethodForInterfaceChecked(
              receiver->GetClass(), called_method, kRuntimePointerSize);
          // WESTLAKE §206: Implements() is NECESSARY BUT NOT SUFFICIENT. FindVirtualMethodForInterface
          // indexes GetMethodArray(i)->Get(called_method->GetMethodIndex()); if that index is foreign
          // to the receiver's iftable entry it returns unrelated heap bytes as a non-null ArtMethod*,
          // and the next instruction (IsProxyMethod -> ldr w8,[x0]) faults. Confirmed by disassembly:
          // the SIGSEGV is at `ldr w8,[x0]` immediately after the call, at the ASCII address
          // 0x006e6f63692f6e6f ("on/icon") -- which is not even 4-byte aligned. Validate before use.
          const uintptr_t wl_pr = reinterpret_cast<uintptr_t>(proxy_replacement);
          const bool wl_sane_ptr = (wl_pr != 0) && ((wl_pr & 3u) == 0) &&
                                   (wl_pr > 0x10000u) && (wl_pr < (1ull << 48));
          if (!wl_sane_ptr) {
            // WESTLAKE §209: skipping (as §206 did) stops the SIGSEGV but leaves the interface call
            // dispatching through an unresolved method. Repair it properly instead: walk the
            // RECEIVER's own class hierarchy for a virtual method matching the name+signature the
            // caller's dex asked for. This needs no iftable indexing, so it is immune to whatever
            // made FindVirtualMethodForInterface return garbage (interface identity mismatch
            // between the caller's dex and the receiver's iftable -- plausible in this port, which
            // injects classes into multiple BCP jars).
            ArtMethod* wl_fix = nullptr;
            for (ObjPtr<mirror::Class> k = receiver->GetClass();
                 k != nullptr && wl_fix == nullptr; k = k->GetSuperClass()) {
              for (ArtMethod& m : k->GetVirtualMethods(kRuntimePointerSize)) {
                if (WlNameMatches(&m, expected_name) && m.GetSignature() == expected_signature) {
                  wl_fix = &m;
                  break;
                }
              }
            }
            static thread_local int wl_bad_pr = 0;
            if (wl_bad_pr < 20) {
              wl_bad_pr++;
              fprintf(stderr, "[WESTLAKE-BADIFACE] bogus %p for %s on receiver %s -> %s\n",
                      reinterpret_cast<void*>(proxy_replacement),
                      called_method->PrettyMethod().c_str(),
                      receiver->GetClass()->PrettyDescriptor().c_str(),
                      wl_fix != nullptr ? wl_fix->PrettyMethod().c_str() : "NO MATCH (left as-is)");
              fflush(stderr);
            }
            if (wl_fix != nullptr) {
              called_method = wl_fix;
            }
          // WESTLAKE §633 (2026-08-15): apply the EXISTING §551 predicate here too.
          // IsProxyMethod() dereferences declaring_class_ (ldr w8,[x23]; ldr w9,[x8,#64];
          // tbz w9,#18). §551/§593 guarded that as a hand-written CODE CAVE in the shipped
          // libart.so only, so every source rebuild silently lost it and §436 came back
          // (measured on a rebuild: sig=11 code=1 addr=0x41/0x45, declaring_class_ == 4/5).
          // The predicate already exists in this file — it just was not applied at this site.
          } else if (PFCutDeclaringClassPlausible(proxy_replacement) &&
                     proxy_replacement->IsProxyMethod()) {
            ArtMethod* proxy_target =
                proxy_replacement->GetInterfaceMethodIfProxy(kRuntimePointerSize);
            if (proxy_target != nullptr &&
                WlNameMatches(proxy_target, expected_name) &&
                proxy_target->GetSignature() == expected_signature) {
              static thread_local int pfc_proxy_interface_repair_count = 0;
              if (pfc_proxy_interface_repair_count < 80) {
                pfc_proxy_interface_repair_count++;
                fprintf(stderr,
                        "[PFCUT-PROXY] repaired interface call %s -> %s\n",
                        called_method->PrettyMethod().c_str(),
                        proxy_replacement->PrettyMethod().c_str());
                fflush(stderr);
              }
              called_method = proxy_replacement;
            }
          }
        }
      }
	      bool signature_mismatch =
	          !WlNameMatches(called_method, expected_name) ||
	          called_method->GetSignature() != expected_signature;
	      if (signature_mismatch && called_method->IsProxyMethod()) {
	        ArtMethod* proxy_target =
	            called_method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
	        if (proxy_target != nullptr &&
	            WlNameMatches(proxy_target, expected_name) &&
	            proxy_target->GetSignature() == expected_signature) {
	          signature_mismatch = false;
	        }
	      }
      if (UNLIKELY(signature_mismatch)) {
        self->GetInterpreterCache()->Clear(self);
        caller->GetDexCache()->SetResolvedMethod(method_idx, nullptr);

        ClassLinker* linker = Runtime::Current()->GetClassLinker();
        ObjPtr<mirror::Class> referenced_class = linker->ResolveType(method_id.class_idx_, caller);
        if (UNLIKELY(referenced_class == nullptr)) {
          DCHECK(self->IsExceptionPending());
          result->SetJ(0);
          return false;
        }

        ArtMethod* replacement = nullptr;
        if (invoke_type == kInterface) {
          ArtMethod* interface_method = referenced_class->FindInterfaceMethod(
              expected_name, expected_signature, kRuntimePointerSize);
          ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(vregC);
          // WESTLAKE 2026-07-22 (§140 FIX, 2nd call site): same unguarded precondition as above --
          // FindVirtualMethodForInterface indexes the receiver's IfTable method array by
          // interface_method->GetMethodIndex(), which is only valid if the receiver actually
          // implements interface_method's declaring interface (DCHECK-only, compiled out).
          ObjPtr<mirror::Class> wl_iface2 =
              (interface_method != nullptr) ? interface_method->GetDeclaringClass() : nullptr;
          const bool wl_ok2 = (receiver != nullptr) && (wl_iface2 != nullptr) &&
                              wl_iface2->IsInterface() &&
                              receiver->GetClass()->Implements(wl_iface2);
          if (wl_ok2) {
            // §673: bounds-checked, same reason as the first call site.
            replacement = WlFindVirtualMethodForInterfaceChecked(
                receiver->GetClass(), interface_method, kRuntimePointerSize);
            if (replacement != nullptr &&
                (!WlNameMatches(replacement, expected_name) ||
                 replacement->GetSignature() != expected_signature)) {
              replacement = receiver->GetClass()->FindClassMethod(
                  expected_name, expected_signature, kRuntimePointerSize);
            }
            if (replacement == nullptr) {
              replacement = receiver->GetClass()->FindClassMethod(
                  expected_name, expected_signature, kRuntimePointerSize);
            }
          }
          if (replacement == nullptr) {
            replacement = interface_method;
          }
        } else if (invoke_type == kVirtual) {
          ObjPtr<mirror::Object> receiver = shadow_frame.GetVRegReference(vregC);
          if (receiver != nullptr) {
            replacement = receiver->GetClass()->FindClassMethod(
                expected_name, expected_signature, kRuntimePointerSize);
          }
          if (replacement == nullptr) {
            replacement = referenced_class->FindClassMethod(
                expected_name, expected_signature, kRuntimePointerSize);
          }
        } else {
          replacement = referenced_class->IsInterface()
              ? referenced_class->FindInterfaceMethod(expected_name,
                                                      expected_signature,
                                                      kRuntimePointerSize)
              : referenced_class->FindClassMethod(expected_name,
                                                  expected_signature,
                                                  kRuntimePointerSize);
        }

	        bool replacement_matches = false;
	        if (replacement != nullptr) {
	          replacement_matches =
	              WlNameMatches(replacement, expected_name) &&
	              replacement->GetSignature() == expected_signature;
	          if (!replacement_matches && replacement->IsProxyMethod()) {
	            ArtMethod* proxy_target =
	                replacement->GetInterfaceMethodIfProxy(kRuntimePointerSize);
	            replacement_matches =
	                proxy_target != nullptr &&
	                WlNameMatches(proxy_target, expected_name) &&
	                proxy_target->GetSignature() == expected_signature;
	          }
	        }
	        if (replacement_matches) {
	          called_method = replacement;
	        } else {
          // WESTLAKE §281d PROBE: statically verified that the dex DOES declare the method this
          // throw claims is absent (IContentProvider.call(AttributionSource,...) is declared
          // `public abstract` in framework.jar classes.dex, mid=29397).  Dump exactly what the
          // lookup saw, instead of inferring why FindInterfaceMethod returned null.
          {
            static thread_local int wl_nsm_probe = 0;
            if (wl_nsm_probe < 8) {
              wl_nsm_probe++;
              const std::string wl_want(expected_name);
              fprintf(stderr,
                      "[WESTLAKE-NSMPROBE] cls=%s iface=%d type=%d want='%s' sig='%s'\n",
                      referenced_class->PrettyDescriptor().c_str(),
                      referenced_class->IsInterface() ? 1 : 0,
                      static_cast<int>(invoke_type),
                      wl_want.c_str(),
                      expected_signature.ToString().c_str());
              int wl_n = 0, wl_same_name = 0;
              for (ArtMethod& wl_m :
                       referenced_class->GetDeclaredMethodsSlice(kRuntimePointerSize)) {
                wl_n++;
                if (WlNameMatches(&wl_m, expected_name)) {
                  wl_same_name++;
                  fprintf(stderr,
                          "[WESTLAKE-NSMPROBE]   declared: %s sig='%s' sigeq=%d\n",
                          wl_m.PrettyMethod().c_str(),
                          wl_m.GetSignature().ToString().c_str(),
                          (wl_m.GetSignature() == expected_signature) ? 1 : 0);
                }
              }
              fprintf(stderr,
                      "[WESTLAKE-NSMPROBE]   declared_total=%d same_name=%d\n",
                      wl_n, wl_same_name);
              fflush(stderr);
            }
          }
          ThrowNoSuchMethodError(invoke_type,
                                 referenced_class,
                                 dex_file->GetStringData(method_id.name_idx_),
                                 expected_signature);
          result->SetJ(0);
          return false;
        }
      }
    }
  }

  // WESTLAKE §740 (2026-08-20): the terminal stock Handler constructor first
  // calls Object.<init>() and then dereferences its p1 Looper. Preserve that
  // caller vreg across the nested constructor only if the nested call changes a
  // non-null reference to null. This is another strict interpreter invariant,
  // not a substitute for an explicitly null Looper.
  const bool wl_handler_terminal_object_call =
      caller_method != nullptr && called_method != nullptr && !is_string_init &&
      PFCutDeclaringClassPlausible(caller_method) &&
      PFCutDeclaringClassPlausible(called_method) &&
      caller_method->GetDeclaringClass()->DescriptorEquals("Landroid/os/Handler;") &&
      strcmp(WlSafeName(caller_method), "<init>") == 0 &&
      caller_method->GetSignature().ToString() ==
          "(Landroid/os/Looper;Landroid/os/Handler$Callback;ZZ)V" &&
      called_method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/Object;") &&
      strcmp(WlSafeName(called_method), "<init>") == 0 &&
      shadow_frame.NumberOfVRegs() >= 5u;
  const uint32_t wl_handler_terminal_looper_reg =
      wl_handler_terminal_object_call ? shadow_frame.NumberOfVRegs() - 4u : 0u;
  ObjPtr<mirror::Object> wl_handler_terminal_looper_before =
      wl_handler_terminal_object_call
          ? shadow_frame.GetVRegReference(wl_handler_terminal_looper_reg) : nullptr;

  // WESTLAKE §741 (2026-08-20): explain why a no-quit HandlerThread becomes
  // non-alive. Looper.loop() should return only after its MessageQueue quits;
  // an app override that rejects both quit APIs should therefore stay here
  // forever. Diagnostic only: report normal return versus a pending exception
  // at the exact managed-call boundary.
  const bool wl_looper_loop_call =
      called_method != nullptr && called_method->IsStatic() && !is_string_init &&
      PFCutDeclaringClassPlausible(called_method) &&
      called_method->GetDeclaringClass()->DescriptorEquals("Landroid/os/Looper;") &&
      strcmp(WlSafeName(called_method), "loop") == 0 &&
      called_method->GetSignature().ToString() == "()V";

  // WESTLAKE §752: trace the exact app-managed handoff around the short-video
  // repository, response Handler, presenter, and response accessors.  The filter is
  // intentionally narrow and the trace is capped per thread because Toutiao executes
  // entirely in the interpreter on this port.
  bool wl_toutiao_video_call = false;
  std::string wl_toutiao_video_caller_desc_storage;
  std::string wl_toutiao_video_callee_desc_storage;
  const char* wl_toutiao_video_caller_desc = "";
  const char* wl_toutiao_video_callee_desc = "";
  const char* wl_toutiao_video_method_name = "";
  char wl_toutiao_video_return_shorty = '?';
  if (WestlakeTraceToutiaoVideoCalls() && caller_method != nullptr &&
      called_method != nullptr && PFCutDeclaringClassPlausible(caller_method) &&
      PFCutDeclaringClassPlausible(called_method)) {
    wl_toutiao_video_caller_desc = caller_method->GetDeclaringClass()->GetDescriptor(
        &wl_toutiao_video_caller_desc_storage);
    wl_toutiao_video_callee_desc = called_method->GetDeclaringClass()->GetDescriptor(
        &wl_toutiao_video_callee_desc_storage);
    wl_toutiao_video_method_name = WlSafeName(called_method);
    const auto exact_video_class = [](const char* descriptor) {
      return descriptor != nullptr &&
          (strcmp(descriptor, "LX/G2V;") == 0 ||
           strcmp(descriptor, "LX/FmZ;") == 0 ||
           strcmp(descriptor, "LX/FmR;") == 0 ||
           strcmp(descriptor, "LX/G2W;") == 0 ||
           strcmp(descriptor, "LX/G0P;") == 0 ||
           strcmp(descriptor, "LX/G05;") == 0 ||
           strcmp(descriptor, "LX/Flq;") == 0 ||
           strcmp(descriptor, "LX/FlB;") == 0 ||
           strcmp(descriptor, "LX/FlH;") == 0 ||
           // WESTLAKE §763: include only the feed parser and its worker runnable.
           // The video request reaches HTTP 200 and constructs protobuf cells, but
           // C0305Flt.i() never delivers the final 10/11 Handler message.  These two
           // classes expose JSONArray.length(), worker indices, parse completion,
           // and the Object.wait()/notifyAll() boundary without tracing the very
           // large CellManager/Wire implementation or changing app behaviour.
           strcmp(descriptor, "LX/Flt;") == 0 ||
           strcmp(descriptor, "LX/Flx;") == 0 ||
           strcmp(descriptor, "LX/FzC;") == 0 ||
           strcmp(descriptor, "LX/G06;") == 0 ||
           strcmp(descriptor, "LX/Fm4;") == 0 ||
           strcmp(descriptor, "LX/FlR;") == 0 ||
           strcmp(descriptor, "LX/FlS;") == 0);
    };
    const bool video_package_caller =
        wl_toutiao_video_caller_desc != nullptr &&
        (strstr(wl_toutiao_video_caller_desc, "Lcom/bytedance/smallvideo/") != nullptr ||
         strstr(wl_toutiao_video_caller_desc, "Lcom/ss/android/ugc/detail/") != nullptr ||
         strstr(wl_toutiao_video_caller_desc, "Lcom/ss/ttvideoengine/") != nullptr);
    const bool response_accessor = wl_toutiao_video_method_name != nullptr &&
        (strcmp(wl_toutiao_video_method_name, "getData") == 0 ||
         strcmp(wl_toutiao_video_method_name, "getErrorCode") == 0 ||
         strcmp(wl_toutiao_video_method_name, "getRequestId") == 0 ||
         strcmp(wl_toutiao_video_method_name, "getTopTime") == 0 ||
         strcmp(wl_toutiao_video_method_name, "getBottomTime") == 0 ||
         strcmp(wl_toutiao_video_method_name, "hasMore") == 0 ||
         strcmp(wl_toutiao_video_method_name, "p0") == 0 ||
         strcmp(wl_toutiao_video_method_name, "P1") == 0);
    static thread_local int wl_toutiao_video_trace_count = 0;
    wl_toutiao_video_call = wl_toutiao_video_trace_count < 5000 &&
        (exact_video_class(wl_toutiao_video_caller_desc) ||
         exact_video_class(wl_toutiao_video_callee_desc) ||
         (video_package_caller && response_accessor));
    if (wl_toutiao_video_call) {
      wl_toutiao_video_trace_count++;
      uint32_t shorty_len = 0;
      const char* shorty = called_method->GetShorty(&shorty_len);
      if (shorty != nullptr && shorty_len > 0u) {
        wl_toutiao_video_return_shorty = shorty[0];
      }
      fprintf(stderr,
              "[WESTLAKE-TTVIDEO-752] enter caller=%s callee=%s inputs=%u range=%d "
              "args=",
              caller_method->PrettyMethod().c_str(),
              called_method->PrettyMethod().c_str(), number_of_inputs,
              is_range ? 1 : 0);
      for (uint32_t i = 0; i < number_of_inputs && i < 8u; ++i) {
        const uint32_t source_vreg = is_range ? vregC + i : arg[i];
        fprintf(stderr, "%s%u:0x%x", i == 0u ? "" : ",", i,
                shadow_frame.GetVReg(source_vreg));
      }
      fprintf(stderr, "\n");
      fflush(stderr);
    }
  }

  bool ok = DoCallCommon<is_range>(
      called_method,
      self,
      shadow_frame,
      result,
      number_of_inputs,
      arg,
      vregC,
      is_string_init);

  if (wl_toutiao_video_call) {
    ObjPtr<mirror::Object> object_result =
        result != nullptr && wl_toutiao_video_return_shorty == 'L'
            ? result->GetL() : nullptr;
    fprintf(stderr,
            "[WESTLAKE-TTVIDEO-752] leave callee=%s ok=%d pending=%d "
            "shorty=%c result=0x%llx object=%p class=%s\n",
            called_method != nullptr ? called_method->PrettyMethod().c_str() : "<null>",
            ok ? 1 : 0, self->IsExceptionPending() ? 1 : 0,
            wl_toutiao_video_return_shorty,
            static_cast<unsigned long long>(result != nullptr ? result->GetJ() : 0),
            object_result.Ptr(),
            object_result != nullptr && object_result->GetClass() != nullptr
                ? object_result->GetClass()->PrettyDescriptor().c_str() : "<none>");
    fflush(stderr);
  }

  if (wl_handler_terminal_object_call) {
    ObjPtr<mirror::Object> looper_after =
        shadow_frame.GetVRegReference(wl_handler_terminal_looper_reg);
    const bool repaired =
        wl_handler_terminal_looper_before != nullptr && looper_after == nullptr;
    if (repaired) {
      shadow_frame.SetVRegReference(wl_handler_terminal_looper_reg,
                                    wl_handler_terminal_looper_before);
    }
    fprintf(stderr,
            "[WESTLAKE-MAINLOOPER-740] before=%p after=%p final=%p repaired=%d\n",
            wl_handler_terminal_looper_before.Ptr(), looper_after.Ptr(),
            shadow_frame.GetVRegReference(wl_handler_terminal_looper_reg),
            repaired ? 1 : 0);
    fflush(stderr);
  }

  if (wl_looper_loop_call) {
    ObjPtr<mirror::Object> exception =
        self->IsExceptionPending() ? self->GetException() : nullptr;
    std::string message_storage;
    const char* message =
        exception != nullptr ? PFCutThrowableMessage(exception, &message_storage) : nullptr;
    fprintf(stderr,
            "[WESTLAKE-LOOPEREXIT-741] caller=%s ok=%d pending=%d exception=%s message=%s\n",
            caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
            ok ? 1 : 0, self->IsExceptionPending() ? 1 : 0,
            exception != nullptr && exception->GetClass() != nullptr
                ? exception->GetClass()->PrettyDescriptor().c_str() : "<none>",
            message != nullptr ? message : "<none>");
    fflush(stderr);
  }

  // WESTLAKE §737 (2026-08-19): preserve the managed-call return from
  // android.os.Looper.getMainLooper().
  //
  // The direct-launch OH boundary measured, through JNI, that
  // Looper.getMainLooper() and Looper.myLooper() are the same live object
  // immediately before Toutiao's TikTokActivity transaction.  The fragment's
  // next ordinary invoke-static of getMainLooper(), however, supplied null to
  // Handler(Looper), which killed the Activity.  The framework method is stock
  // and simply returns the synchronized sMainLooper field.  Check the managed
  // invocation against that exact field here: a null method result while the
  // field is non-null is an ART interpreter return-path violation, not an
  // Android state transition.  Repair only that impossible mismatch; a
  // legitimately unprepared process (both null) is left unchanged.
  if (called_method != nullptr && called_method->IsStatic() && !is_string_init &&
      PFCutDeclaringClassPlausible(called_method) &&
      called_method->GetDeclaringClass()->DescriptorEquals("Landroid/os/Looper;") &&
      strcmp(WlSafeName(called_method), "getMainLooper") == 0) {
    ObjPtr<mirror::Object> static_main = nullptr;
    for (ArtField& field : called_method->GetDeclaringClass()->GetSFields()) {
      if (strcmp(field.GetName(), "sMainLooper") == 0) {
        static_main = field.GetObject(called_method->GetDeclaringClass());
        break;
      }
    }
    ObjPtr<mirror::Object> managed_result = result != nullptr ? result->GetL() : nullptr;
    const bool repaired = ok && !self->IsExceptionPending() && result != nullptr &&
                          managed_result == nullptr && static_main != nullptr;
    if (repaired) {
      result->SetL(static_main);
    }
    g_westlake_last_main_looper_method = called_method;
    g_westlake_last_main_looper_caller = caller_method;
    g_westlake_last_main_looper_dex_pc = shadow_frame.GetDexPC();
    g_westlake_last_main_looper_non_null =
        result != nullptr && result->GetL() != nullptr && !self->IsExceptionPending();
    const int wl_main_looper_return_count =
        g_westlake_main_looper_return_logs.fetch_add(1, std::memory_order_relaxed);
    if (wl_main_looper_return_count < 80) {
      fprintf(stderr,
              "[WESTLAKE-MAINLOOPER-737] caller=%s result=%p static=%p "
              "ok=%d pending=%d repaired=%d\n",
              caller_method != nullptr ? caller_method->PrettyMethod().c_str() : "<null>",
              managed_result.Ptr(), static_main.Ptr(), ok ? 1 : 0,
              self->IsExceptionPending() ? 1 : 0, repaired ? 1 : 0);
      fflush(stderr);
    }
  }
  if (pfc_diag) {
    fprintf(stderr,
            "[PFCUT] DoCall after DoCallCommon dexIdx=%u ok=%d pending=%d top_shadow=%p top_quick=%p\n",
            called_method->GetDexMethodIndex(),
            ok ? 1 : 0,
            self->IsExceptionPending() ? 1 : 0,
            self->GetManagedStack()->GetTopShadowFrame(),
            self->GetManagedStack()->GetTopQuickFrame());
    fflush(stderr);
  }
  return ok;
}

template <bool is_range>
bool DoFilledNewArray(const Instruction* inst,
                      const ShadowFrame& shadow_frame,
                      Thread* self,
                      JValue* result) {
  DCHECK(inst->Opcode() == Instruction::FILLED_NEW_ARRAY ||
         inst->Opcode() == Instruction::FILLED_NEW_ARRAY_RANGE);
  const int32_t length = is_range ? inst->VRegA_3rc() : inst->VRegA_35c();
  if (!is_range) {
    // Checks FILLED_NEW_ARRAY's length does not exceed 5 arguments.
    CHECK_LE(length, 5);
  }
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return false;
  }
  uint16_t type_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  bool do_access_check = !shadow_frame.GetMethod()->SkipAccessChecks();
  ObjPtr<mirror::Class> array_class = ResolveVerifyAndClinit(dex::TypeIndex(type_idx),
                                                             shadow_frame.GetMethod(),
                                                             self,
                                                             false,
                                                             do_access_check);
  if (UNLIKELY(array_class == nullptr)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  CHECK(array_class->IsArrayClass());
  ObjPtr<mirror::Class> component_class = array_class->GetComponentType();
  const bool is_primitive_int_component = component_class->IsPrimitiveInt();
  if (UNLIKELY(component_class->IsPrimitive() && !is_primitive_int_component)) {
    if (component_class->IsPrimitiveLong() || component_class->IsPrimitiveDouble()) {
      ThrowRuntimeException("Bad filled array request for type %s",
                            component_class->PrettyDescriptor().c_str());
    } else {
      self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                               "Found type %s; filled-new-array not implemented for anything but 'int'",
                               component_class->PrettyDescriptor().c_str());
    }
    return false;
  }
  ObjPtr<mirror::Object> new_array = mirror::Array::Alloc(
      self,
      array_class,
      length,
      array_class->GetComponentSizeShift(),
      Runtime::Current()->GetHeap()->GetCurrentAllocator());
  if (UNLIKELY(new_array == nullptr)) {
    self->AssertPendingOOMException();
    return false;
  }
  uint32_t arg[Instruction::kMaxVarArgRegs];  // only used in filled-new-array.
  uint32_t vregC = 0;   // only used in filled-new-array-range.
  if (is_range) {
    vregC = inst->VRegC_3rc();
  } else {
    inst->GetVarArgs(arg);
  }
  // We're initializing a newly allocated array, so we do not need to record that under
  // a transaction. If the transaction is aborted, the whole array shall be unreachable.
  if (LIKELY(is_primitive_int_component)) {
    ObjPtr<mirror::IntArray> int_array = new_array->AsIntArray();
    for (int32_t i = 0; i < length; ++i) {
      size_t src_reg = is_range ? vregC + i : arg[i];
      int_array->SetWithoutChecks</*kTransactionActive=*/ false, /*kCheckTransaction=*/ false>(
          i, shadow_frame.GetVReg(src_reg));
    }
  } else {
    ObjPtr<mirror::ObjectArray<mirror::Object>> object_array =
        new_array->AsObjectArray<mirror::Object>();
    for (int32_t i = 0; i < length; ++i) {
      size_t src_reg = is_range ? vregC + i : arg[i];
      object_array->SetWithoutChecks</*kTransactionActive=*/ false, /*kCheckTransaction=*/ false>(
          i, shadow_frame.GetVRegReference(src_reg));
    }
  }

  result->SetL(new_array);
  return true;
}

void UnlockHeldMonitors(Thread* self, ShadowFrame* shadow_frame)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(shadow_frame->GetForcePopFrame() ||
         (Runtime::Current()->IsActiveTransaction() &&
             Runtime::Current()->GetClassLinker()->IsTransactionAborted()));
  // Unlock all monitors.
  if (shadow_frame->GetMethod()->MustCountLocks()) {
    DCHECK(!shadow_frame->GetMethod()->SkipAccessChecks());
    // Get the monitors from the shadow-frame monitor-count data.
    shadow_frame->GetLockCountData().VisitMonitors(
      [&](mirror::Object** obj) REQUIRES_SHARED(Locks::mutator_lock_) {
        // Since we don't use the 'obj' pointer after the DoMonitorExit everything should be fine
        // WRT suspension.
        DoMonitorExit(self, shadow_frame, *obj);
      });
  } else {
    std::vector<verifier::MethodVerifier::DexLockInfo> locks;
    verifier::MethodVerifier::FindLocksAtDexPc(shadow_frame->GetMethod(),
                                               shadow_frame->GetDexPC(),
                                               &locks,
                                               Runtime::Current()->GetTargetSdkVersion());
    for (const auto& reg : locks) {
      if (UNLIKELY(reg.dex_registers.empty())) {
        LOG(ERROR) << "Unable to determine reference locked by "
                   << shadow_frame->GetMethod()->PrettyMethod() << " at pc "
                   << shadow_frame->GetDexPC();
      } else {
        DoMonitorExit(
            self, shadow_frame, shadow_frame->GetVRegReference(*reg.dex_registers.begin()));
      }
    }
  }
}

void PerformNonStandardReturn(Thread* self,
                              ShadowFrame& frame,
                              JValue& result,
                              const instrumentation::Instrumentation* instrumentation,
                              bool unlock_monitors) {
  if (UNLIKELY(self->IsExceptionPending())) {
    LOG(WARNING) << "Suppressing exception for non-standard method exit: "
                 << self->GetException()->Dump();
    self->ClearException();
  }
  if (unlock_monitors) {
    UnlockHeldMonitors(self, &frame);
    DoMonitorCheckOnExit(self, &frame);
  }
  result = JValue();
  if (UNLIKELY(NeedsMethodExitEvent(instrumentation))) {
    SendMethodExitEvents(self, instrumentation, frame, frame.GetMethod(), result);
  }
}

// Explicit DoCall template function declarations.
#define EXPLICIT_DO_CALL_TEMPLATE_DECL(_is_range)                      \
  template REQUIRES_SHARED(Locks::mutator_lock_)                       \
  bool DoCall<_is_range>(ArtMethod* method,                            \
                         Thread* self,                                 \
                         ShadowFrame& shadow_frame,                    \
                         const Instruction* inst,                      \
                         uint16_t inst_data,                           \
                         bool string_init,                             \
                         JValue* result)
EXPLICIT_DO_CALL_TEMPLATE_DECL(false);
EXPLICIT_DO_CALL_TEMPLATE_DECL(true);
#undef EXPLICIT_DO_CALL_TEMPLATE_DECL

// Explicit DoInvokePolymorphic template function declarations.
#define EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL(_is_range)          \
  template REQUIRES_SHARED(Locks::mutator_lock_)                         \
  bool DoInvokePolymorphic<_is_range>(                                   \
      Thread* self, ShadowFrame& shadow_frame, const Instruction* inst,  \
      uint16_t inst_data, JValue* result)
EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL(false);
EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL(true);
#undef EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL

// Explicit DoFilledNewArray template function declarations.
#define EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(_is_range_)               \
  template REQUIRES_SHARED(Locks::mutator_lock_)                             \
  bool DoFilledNewArray<_is_range_>(const Instruction* inst,                 \
                                    const ShadowFrame& shadow_frame,         \
                                    Thread* self,                            \
                                    JValue* result)
EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false);
EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true);
#undef EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL

}  // namespace interpreter
}  // namespace art
