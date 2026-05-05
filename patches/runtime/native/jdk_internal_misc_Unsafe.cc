/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "jdk_internal_misc_Unsafe.h"

#include <unistd.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <limits>

#include "nativehelper/jni_macros.h"

#include "base/quasi_atomic.h"
#include "common_throws.h"
#include "gc/accounting/card_table-inl.h"
#include "jni/jni_internal.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "art_field-inl.h"
#include "tolerant_native_util.h"
#include "scoped_fast_native_object_access-inl.h"
#include "well_known_classes-inl.h"

namespace art HIDDEN {

extern bool PFCutAppClassLoaderSeen();

// PF-noice-001 (2026-05-04) trace API defined in runtime.cc.
extern bool PFCutTraceActive();
extern void PFCutTraceUnsafeArrayWrite(const char* kind,
                                        ObjPtr<mirror::Object> array_obj,
                                        int32_t array_index,
                                        int32_t array_length,
                                        ObjPtr<mirror::Class> array_class,
                                        ObjPtr<mirror::Class> component_class,
                                        ObjPtr<mirror::Object> value,
                                        ObjPtr<mirror::Class> value_class,
                                        bool assignable)
    REQUIRES_SHARED(Locks::mutator_lock_);

namespace {
  bool PFCutIsBogusUnsafeObject(ObjPtr<mirror::Object> value) {
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(value.Ptr());
    static constexpr uintptr_t kPFCutStaleEntry64 = 0xfffffffffffffb17ULL;
    static constexpr uintptr_t kPFCutStaleEntry32 = 0xfffffb17ULL;
    static constexpr uintptr_t kPFCutMinObjectAlignment = 4u;
    return ptr == kPFCutStaleEntry64 || ptr == kPFCutStaleEntry32 ||
           (ptr != 0u && ptr < 0x10000u) ||
           ((ptr & (kPFCutMinObjectAlignment - 1u)) != 0u);
  }

  bool PFCutIsBogusUnsafeOffset(jlong offset) {
    static constexpr jlong kPFCutMaxReasonableOffset = 1LL << 30;
    static constexpr jlong kPFCutStaleEntry32 = 0xfffffb17LL;
    return offset < 0 || offset > kPFCutMaxReasonableOffset || offset == kPFCutStaleEntry32 ||
           ((offset % static_cast<jlong>(kHeapReferenceSize)) != 0);
  }

  bool PFCutIsBogusUnsafeJObject(jobject value) {
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(value);
    static constexpr uintptr_t kPFCutStaleEntry64 = 0xfffffffffffffb17ULL;
    static constexpr uintptr_t kPFCutStaleEntry32 = 0xfffffb17ULL;
    // A JNI jobject is an indirect handle, not a decoded mirror::Object*
    // address. It can legitimately be tagged/unaligned. Alignment must be
    // checked only after ScopedFastNativeObjectAccess decodes the handle.
    return ptr == kPFCutStaleEntry64 || ptr == kPFCutStaleEntry32 ||
           (ptr != 0u && ptr < 0x10000u);
  }

  bool PFCutRejectUnsafeJObject(const char* method, jobject javaObj, jlong offset) {
    if (UNLIKELY(javaObj == nullptr ||
                 PFCutIsBogusUnsafeJObject(javaObj) ||
                 PFCutIsBogusUnsafeOffset(offset))) {
      static thread_local int bogus_jobject_count = 0;
      if (bogus_jobject_count < 80) {
        bogus_jobject_count++;
        fprintf(stderr,
                "[PFCUT] jdk.Unsafe bogus jobject %s javaObj=%p offset=%lld\n",
                method,
                javaObj,
                static_cast<long long>(offset));
        fflush(stderr);
      }
      return true;
    }
    return false;
  }

  void PFCutLogUnsafeBogusObject(const char* method,
                                 ObjPtr<mirror::Object> obj,
                                 jlong offset,
                                 ObjPtr<mirror::Object> value) {
    static thread_local int bogus_object_count = 0;
    if (bogus_object_count < 80) {
      bogus_object_count++;
      fprintf(stderr,
              "[PFCUT] jdk.Unsafe bogus object %s obj=%p offset=%lld value=%p\n",
              method,
              obj.Ptr(),
              static_cast<long long>(offset),
              value.Ptr());
      fflush(stderr);
    }
  }

  bool PFCutRejectUnsafeAccess(const char* method,
                               ObjPtr<mirror::Object> obj,
                               jlong offset) {
    if (UNLIKELY(obj == nullptr ||
                 PFCutIsBogusUnsafeObject(obj) ||
                 PFCutIsBogusUnsafeOffset(offset))) {
      PFCutLogUnsafeBogusObject(method, obj, offset, nullptr);
      return true;
    }
    return false;
  }

  // PF-630 boot-aware routing gate (2026-05-04). Defined in runtime.cc.
  // While false (i.e. before the app PathClassLoader is installed), PFCut's
  // array-backed Unsafe path is bypassed and Unsafe ops fall through to
  // stock CasFieldObject/GetFieldObject{Volatile}/SetFieldObject{Volatile}.
  bool PFCutObjectArrayIndexFromOffset(ObjPtr<mirror::Object> obj,
                                       jlong offset,
                                       int32_t* index_out) {
    if (UNLIKELY(!::art::PFCutAppClassLoaderSeen())) {
      return false;
    }
    if (obj == nullptr || !obj->IsObjectArray()) {
      return false;
    }
    const int32_t base = mirror::Array::DataOffset(kHeapReferenceSize).Int32Value();
    if (offset < base) {
      return false;
    }
    const jlong delta = offset - base;
    if ((delta % kHeapReferenceSize) != 0) {
      return false;
    }
    const jlong index64 = delta / kHeapReferenceSize;
    ObjPtr<mirror::ObjectArray<mirror::Object>> array =
        obj->AsObjectArray<mirror::Object>();
    if (index64 < 0 || index64 > static_cast<jlong>(std::numeric_limits<int32_t>::max()) ||
        index64 >= array->GetLength()) {
      return false;
    }
    *index_out = static_cast<int32_t>(index64);
    return true;
  }

  ObjPtr<mirror::Object> PFCutUnsafeGetObjectArraySlot(ObjPtr<mirror::Object> obj,
                                                       int32_t index) {
    ObjPtr<mirror::Object> result =
        obj->AsObjectArray<mirror::Object>()->GetWithoutChecks(index);
    if (UNLIKELY(::art::PFCutTraceActive())) {
      ObjPtr<mirror::ObjectArray<mirror::Object>> array =
          obj->AsObjectArray<mirror::Object>();
      ObjPtr<mirror::Class> array_class = array->GetClass();
      ObjPtr<mirror::Class> component_class =
          array_class != nullptr ? array_class->GetComponentType() : nullptr;
      ObjPtr<mirror::Class> value_class =
          result != nullptr ? result->GetClass() : nullptr;
      bool assignable = (component_class == nullptr || value_class == nullptr) ||
                        component_class->IsAssignableFrom(value_class.Ptr());
      ::art::PFCutTraceUnsafeArrayWrite("getSlot.jdk", obj, index, array->GetLength(),
                                        array_class, component_class,
                                        result, value_class, assignable);
    }
    return result;
  }

  void PFCutUnsafeSetObjectArraySlot(ObjPtr<mirror::Object> obj,
                                     int32_t index,
                                     ObjPtr<mirror::Object> value) {
    if (UNLIKELY(::art::PFCutTraceActive())) {
      ObjPtr<mirror::ObjectArray<mirror::Object>> array =
          obj->AsObjectArray<mirror::Object>();
      ObjPtr<mirror::Class> array_class = array->GetClass();
      ObjPtr<mirror::Class> component_class =
          array_class != nullptr ? array_class->GetComponentType() : nullptr;
      ObjPtr<mirror::Class> value_class =
          value != nullptr ? value->GetClass() : nullptr;
      bool assignable = (component_class == nullptr || value_class == nullptr) ||
                        component_class->IsAssignableFrom(value_class.Ptr());
      ::art::PFCutTraceUnsafeArrayWrite("setSlot.jdk", obj, index, array->GetLength(),
                                        array_class, component_class,
                                        value, value_class, assignable);
    }
    obj->AsObjectArray<mirror::Object>()->SetWithoutChecks</*kTransactionActive=*/ false,
                                                 /*kCheckTransaction=*/ false>(index, value);
  }

  void PFCutLogUnsafeCasInt(ObjPtr<mirror::Object> obj,
                            jlong offset,
                            jint current,
                            jint expected,
                            jint new_value,
                            bool success) {
    static thread_local int cas_int_count = 0;
    if (cas_int_count < 96) {
      cas_int_count++;
      fprintf(stderr,
              "[PFCUT] jdk.Unsafe CAS int obj=%p offset=%lld current=%d expected=%d new=%d success=%d\n",
              obj.Ptr(),
              static_cast<long long>(offset),
              current,
              expected,
              new_value,
              success ? 1 : 0);
      fflush(stderr);
    }
  }

  // Checks a JNI argument `size` fits inside a size_t and throws a RuntimeException if not (see
  // jdk/internal/misc/Unsafe.java comments).
  bool ValidJniSizeArgument(jlong size) REQUIRES_SHARED(Locks::mutator_lock_) {
    const jlong maybe_truncated_size = static_cast<jlong>(static_cast<size_t>(size));
    // size is nonnegative and fits into size_t
    if (LIKELY(size >= 0 && size == maybe_truncated_size)) {
      return true;
    }
    ThrowRuntimeException("Bad size: %" PRIu64, size);
    return false;
  }
}  // namespace

static jboolean Unsafe_compareAndSetInt(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                        jint expectedValue, jint newValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("compareAndSetInt.raw", javaObj, offset))) {
    return JNI_FALSE;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("compareAndSetInt", obj, offset))) {
    return JNI_FALSE;
  }
  MemberOffset member_offset(offset);
  jint current = obj->GetField32Volatile(member_offset);
  const bool success = obj->CasField32<false>(member_offset,
                                              expectedValue,
                                              newValue,
                                              CASMode::kStrong,
                                              std::memory_order_seq_cst);
  if (!success) {
    current = obj->GetField32Volatile(member_offset);
  }
  PFCutLogUnsafeCasInt(obj, offset, current, expectedValue, newValue, success);
  return success ? JNI_TRUE : JNI_FALSE;
}

static jboolean Unsafe_compareAndSwapInt(JNIEnv* env, jobject obj, jobject javaObj, jlong offset,
                                         jint expectedValue, jint newValue) {
  // compareAndSetInt has the same semantics as compareAndSwapInt, except for
  // being strict (volatile). Since this was implemented in a strict mode it can
  // just call the volatile version unless it gets relaxed.
  return Unsafe_compareAndSetInt(env, obj, javaObj, offset, expectedValue, newValue);
}

static jboolean Unsafe_compareAndSetLong(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                         jlong expectedValue, jlong newValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("compareAndSetLong.raw", javaObj, offset))) {
    return JNI_FALSE;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("compareAndSetLong", obj, offset))) {
    return JNI_FALSE;
  }
  MemberOffset member_offset(offset);
  const bool success = obj->CasFieldStrongSequentiallyConsistent64<false>(member_offset,
                                                                         expectedValue,
                                                                         newValue);
  if (!success) {
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

static jboolean Unsafe_compareAndSwapLong(JNIEnv* env, jobject obj, jobject javaObj, jlong offset,
                                          jlong expectedValue, jlong newValue) {
  // compareAndSetLong has the same semantics as compareAndSwapLong, except for
  // being strict (volatile). Since this was implemented in a strict mode it can
  // just call the volatile version unless it gets relaxed.
  return Unsafe_compareAndSetLong(env, obj, javaObj, offset, expectedValue, newValue);
}

static jboolean Unsafe_compareAndSetReference(JNIEnv* env,
                                              jobject,
                                              jobject javaObj,
                                              jlong offset,
                                              jobject javaExpectedValue,
                                              jobject javaNewValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("compareAndSetReference.raw", javaObj, offset))) {
    return JNI_FALSE;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("compareAndSetReference", obj, offset))) {
    return JNI_FALSE;
  }
  ObjPtr<mirror::Object> expectedValue = soa.Decode<mirror::Object>(javaExpectedValue);
  ObjPtr<mirror::Object> newValue = soa.Decode<mirror::Object>(javaNewValue);
  if (UNLIKELY(PFCutIsBogusUnsafeObject(expectedValue) ||
               PFCutIsBogusUnsafeObject(newValue))) {
    PFCutLogUnsafeBogusObject("compareAndSetReference.arg", obj, offset,
                              PFCutIsBogusUnsafeObject(newValue) ? newValue : expectedValue);
    return JNI_FALSE;
  }
  int32_t array_index = -1;
  if (PFCutObjectArrayIndexFromOffset(obj, offset, &array_index)) {
    ObjPtr<mirror::Object> current = PFCutUnsafeGetObjectArraySlot(obj, array_index);
    if (UNLIKELY(PFCutIsBogusUnsafeObject(current))) {
      PFCutLogUnsafeBogusObject("compareAndSetReference.array", obj, offset, current);
      return JNI_FALSE;
    }
    return obj->CasFieldObject<false>(MemberOffset(offset),
                                      expectedValue,
                                      newValue,
                                      CASMode::kStrong,
                                      std::memory_order_seq_cst) ? JNI_TRUE : JNI_FALSE;
  }
  if (gUseReadBarrier) {
    // Need to make sure the reference stored in the field is a to-space one before attempting the
    // CAS or the CAS could fail incorrectly.
    // Note that the read barrier load does NOT need to be volatile.
    mirror::HeapReference<mirror::Object>* field_addr =
        reinterpret_cast<mirror::HeapReference<mirror::Object>*>(
            reinterpret_cast<uint8_t*>(obj.Ptr()) + static_cast<size_t>(offset));
    ReadBarrier::Barrier<mirror::Object, /*kIsVolatile=*/ false, kWithReadBarrier,
        /* kAlwaysUpdateField= */ true>(
        obj.Ptr(),
        MemberOffset(offset),
        field_addr);
  }
  MemberOffset member_offset(offset);
  ObjPtr<mirror::Object> current =
      obj->GetFieldObjectVolatile<mirror::Object>(member_offset);
  if (UNLIKELY(PFCutIsBogusUnsafeObject(current))) {
    PFCutLogUnsafeBogusObject("compareAndSetReference", obj, offset, current);
    return JNI_FALSE;
  }
  return obj->CasFieldObject<false>(member_offset,
                                    expectedValue,
                                    newValue,
                                    CASMode::kStrong,
                                    std::memory_order_seq_cst) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Unsafe_compareAndSwapObject(JNIEnv* env, jobject obj, jobject javaObj, jlong offset,
                                            jobject javaExpectedValue, jobject javaNewValue) {
  // compareAndSetReference has the same semantics as compareAndSwapObject, except for
  // being strict (volatile). Since this was implemented in a strict mode it can
  // just call the volatile version unless it gets relaxed.
  return Unsafe_compareAndSetReference(env, obj, javaObj, offset, javaExpectedValue, javaNewValue);
}

static jint Unsafe_getInt(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("getInt.raw", javaObj, offset))) {
    return 0;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("getInt", obj, offset))) {
    return 0;
  }
  return obj->GetField32(MemberOffset(offset));
}

static jint Unsafe_getIntVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("getIntVolatile.raw", javaObj, offset))) {
    return 0;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("getIntVolatile", obj, offset))) {
    return 0;
  }
  return obj->GetField32Volatile(MemberOffset(offset));
}

static void Unsafe_putInt(JNIEnv* env, jobject, jobject javaObj, jlong offset, jint newValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("putInt.raw", javaObj, offset))) {
    return;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("putInt", obj, offset))) {
    return;
  }
  // JNI must use non transactional mode.
  obj->SetField32<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putIntVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                  jint newValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("putIntVolatile.raw", javaObj, offset))) {
    return;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("putIntVolatile", obj, offset))) {
    return;
  }
  // JNI must use non transactional mode.
  obj->SetField32Volatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedInt(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                 jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  // TODO: A release store is likely to be faster on future processors.
  std::atomic_thread_fence(std::memory_order_release);
  // JNI must use non transactional mode.
  obj->SetField32<false>(MemberOffset(offset), newValue);
}

static jlong Unsafe_getLong(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("getLong.raw", javaObj, offset))) {
    return 0;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("getLong", obj, offset))) {
    return 0;
  }
  return obj->GetField64(MemberOffset(offset));
}

static jlong Unsafe_getLongVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("getLongVolatile.raw", javaObj, offset))) {
    return 0;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("getLongVolatile", obj, offset))) {
    return 0;
  }
  return obj->GetField64Volatile(MemberOffset(offset));
}

static void Unsafe_putLong(JNIEnv* env, jobject, jobject javaObj, jlong offset, jlong newValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("putLong.raw", javaObj, offset))) {
    return;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("putLong", obj, offset))) {
    return;
  }
  // JNI must use non transactional mode.
  obj->SetField64<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putLongVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                   jlong newValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("putLongVolatile.raw", javaObj, offset))) {
    return;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("putLongVolatile", obj, offset))) {
    return;
  }
  // JNI must use non transactional mode.
  obj->SetField64Volatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedLong(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                  jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  std::atomic_thread_fence(std::memory_order_release);
  // JNI must use non transactional mode.
  obj->SetField64<false>(MemberOffset(offset), newValue);
}

static jobject Unsafe_getReferenceVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("getReferenceVolatile.raw", javaObj, offset))) {
    return nullptr;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("getReferenceVolatile", obj, offset))) {
    return nullptr;
  }
  int32_t array_index = -1;
  if (PFCutObjectArrayIndexFromOffset(obj, offset, &array_index)) {
    ObjPtr<mirror::Object> array_value = PFCutUnsafeGetObjectArraySlot(obj, array_index);
    if (UNLIKELY(PFCutIsBogusUnsafeObject(array_value))) {
      PFCutLogUnsafeBogusObject("getReferenceVolatile.array", obj, offset, array_value);
      return nullptr;
    }
    return soa.AddLocalReference<jobject>(array_value);
  }
  ObjPtr<mirror::Object> value = obj->GetFieldObjectVolatile<mirror::Object>(MemberOffset(offset));
  if (UNLIKELY(PFCutIsBogusUnsafeObject(value))) {
    PFCutLogUnsafeBogusObject("getReferenceVolatile", obj, offset, value);
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(value);
}

static jobject Unsafe_getReference(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("getReference.raw", javaObj, offset))) {
    return nullptr;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  if (UNLIKELY(PFCutRejectUnsafeAccess("getReference", obj, offset))) {
    return nullptr;
  }
  int32_t array_index = -1;
  if (PFCutObjectArrayIndexFromOffset(obj, offset, &array_index)) {
    ObjPtr<mirror::Object> array_value = PFCutUnsafeGetObjectArraySlot(obj, array_index);
    if (UNLIKELY(PFCutIsBogusUnsafeObject(array_value))) {
      PFCutLogUnsafeBogusObject("getReference.array", obj, offset, array_value);
      return nullptr;
    }
    return soa.AddLocalReference<jobject>(array_value);
  }
  ObjPtr<mirror::Object> value = obj->GetFieldObject<mirror::Object>(MemberOffset(offset));
  if (UNLIKELY(PFCutIsBogusUnsafeObject(value))) {
    PFCutLogUnsafeBogusObject("getReference", obj, offset, value);
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(value);
}

static void Unsafe_putReference(
    JNIEnv* env, jobject, jobject javaObj, jlong offset, jobject javaNewValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("putReference.raw", javaObj, offset))) {
    return;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  ObjPtr<mirror::Object> newValue = soa.Decode<mirror::Object>(javaNewValue);
  if (UNLIKELY(PFCutRejectUnsafeAccess("putReference", obj, offset) ||
               PFCutIsBogusUnsafeObject(newValue))) {
    PFCutLogUnsafeBogusObject("putReference.value", obj, offset, newValue);
    return;
  }
  int32_t array_index = -1;
  if (PFCutObjectArrayIndexFromOffset(obj, offset, &array_index)) {
    PFCutUnsafeSetObjectArraySlot(obj, array_index, newValue);
    return;
  }
  // JNI must use non transactional mode.
  obj->SetFieldObject<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putReferenceVolatile(
    JNIEnv* env, jobject, jobject javaObj, jlong offset, jobject javaNewValue) {
  if (UNLIKELY(PFCutRejectUnsafeJObject("putReferenceVolatile.raw", javaObj, offset))) {
    return;
  }
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  ObjPtr<mirror::Object> newValue = soa.Decode<mirror::Object>(javaNewValue);
  if (UNLIKELY(PFCutRejectUnsafeAccess("putReferenceVolatile", obj, offset) ||
               PFCutIsBogusUnsafeObject(newValue))) {
    PFCutLogUnsafeBogusObject("putReferenceVolatile.value", obj, offset, newValue);
    return;
  }
  int32_t array_index = -1;
  if (PFCutObjectArrayIndexFromOffset(obj, offset, &array_index)) {
    PFCutUnsafeSetObjectArraySlot(obj, array_index, newValue);
    return;
  }
  // JNI must use non transactional mode.
  obj->SetFieldObjectVolatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedObject(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                    jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  ObjPtr<mirror::Object> newValue = soa.Decode<mirror::Object>(javaNewValue);
  std::atomic_thread_fence(std::memory_order_release);
  // JNI must use non transactional mode.
  obj->SetFieldObject<false>(MemberOffset(offset), newValue);
}

// Native implementation of objectFieldOffset(Class, String) — bypasses Java version
// which calls Field.getName() and fails during early class init.
// NOT static — needs to be accessible from runtime.cc for direct registration.
jlong Unsafe_objectFieldOffsetClassString(JNIEnv* env, jobject, jclass javaClass, jstring javaName) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Class> klass = soa.Decode<mirror::Class>(javaClass);
  ObjPtr<mirror::String> name = soa.Decode<mirror::String>(javaName);
  if (klass == nullptr || name == nullptr) {
    ThrowNullPointerException("objectFieldOffset: null class or name");
    return 0;
  }
  // Convert Java String to C string
  std::string name_str = name->ToModifiedUtf8();
  // Search instance fields
  for (uint32_t i = 0; i < klass->NumInstanceFields(); i++) {
    ArtField* field = klass->GetInstanceField(i);
    if (name_str == field->GetName()) {
      return field->GetOffset().SizeValue();
    }
  }
  // Search static fields
  for (uint32_t i = 0; i < klass->NumStaticFields(); i++) {
    ArtField* field = klass->GetStaticField(i);
    if (name_str == field->GetName()) {
      return field->GetOffset().SizeValue();
    }
  }
  soa.Self()->ThrowNewException("Ljava/lang/InternalError;",
           (std::string("Field not found: ") + name_str).c_str());
  return 0;
}

static jint Unsafe_getArrayBaseOffsetForComponentType(JNIEnv* env, jclass, jclass component_class) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Class> component = soa.Decode<mirror::Class>(component_class);
  Primitive::Type primitive_type = component->GetPrimitiveType();
  return mirror::Array::DataOffset(Primitive::ComponentSize(primitive_type)).Int32Value();
}

static jint Unsafe_getArrayIndexScaleForComponentType(JNIEnv* env, jclass, jclass component_class) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Class> component = soa.Decode<mirror::Class>(component_class);
  Primitive::Type primitive_type = component->GetPrimitiveType();
  return Primitive::ComponentSize(primitive_type);
}

static jint Unsafe_addressSize([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject ob) {
  return sizeof(void*);
}

static jint Unsafe_pageSize([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject ob) {
  return sysconf(_SC_PAGESIZE);
}

static jlong Unsafe_allocateMemory(JNIEnv* env, jobject, jlong bytes) {
  ScopedFastNativeObjectAccess soa(env);
  if (bytes == 0) {
    return 0;
  }
  // bytes is nonnegative and fits into size_t
  if (!ValidJniSizeArgument(bytes)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return 0;
  }
  const size_t malloc_bytes = static_cast<size_t>(bytes);
  void* mem = malloc(malloc_bytes);
  if (mem == nullptr) {
    soa.Self()->ThrowOutOfMemoryError("native alloc");
    return 0;
  }
  return reinterpret_cast<uintptr_t>(mem);
}

static void Unsafe_freeMemory([[maybe_unused]] JNIEnv* env, jobject, jlong address) {
  free(reinterpret_cast<void*>(static_cast<uintptr_t>(address)));
}

static void Unsafe_setMemory(
    [[maybe_unused]] JNIEnv* env, jobject, jlong address, jlong bytes, jbyte value) {
  memset(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), value, bytes);
}

static jbyte Unsafe_getByteJ([[maybe_unused]] JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jbyte*>(address);
}

static void Unsafe_putByteJB([[maybe_unused]] JNIEnv* env, jobject, jlong address, jbyte value) {
  *reinterpret_cast<jbyte*>(address) = value;
}

static jshort Unsafe_getShortJ([[maybe_unused]] JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jshort*>(address);
}

static void Unsafe_putShortJS([[maybe_unused]] JNIEnv* env, jobject, jlong address, jshort value) {
  *reinterpret_cast<jshort*>(address) = value;
}

static jchar Unsafe_getCharJ([[maybe_unused]] JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jchar*>(address);
}

static void Unsafe_putCharJC([[maybe_unused]] JNIEnv* env, jobject, jlong address, jchar value) {
  *reinterpret_cast<jchar*>(address) = value;
}

static jint Unsafe_getIntJ([[maybe_unused]] JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jint*>(address);
}

static void Unsafe_putIntJI([[maybe_unused]] JNIEnv* env, jobject, jlong address, jint value) {
  *reinterpret_cast<jint*>(address) = value;
}

static jlong Unsafe_getLongJ([[maybe_unused]] JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jlong*>(address);
}

static void Unsafe_putLongJJ([[maybe_unused]] JNIEnv* env, jobject, jlong address, jlong value) {
  *reinterpret_cast<jlong*>(address) = value;
}

static jfloat Unsafe_getFloatJ([[maybe_unused]] JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jfloat*>(address);
}

static void Unsafe_putFloatJF([[maybe_unused]] JNIEnv* env, jobject, jlong address, jfloat value) {
  *reinterpret_cast<jfloat*>(address) = value;
}
static jdouble Unsafe_getDoubleJ([[maybe_unused]] JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jdouble*>(address);
}

static void Unsafe_putDoubleJD([[maybe_unused]] JNIEnv* env,
                               jobject,
                               jlong address,
                               jdouble value) {
  *reinterpret_cast<jdouble*>(address) = value;
}

static void Unsafe_copyMemory0(JNIEnv* env,
                               [[maybe_unused]] jobject unsafe,
                               jobject srcObj,
                               jlong srcOffset,
                               jobject dstObj,
                               jlong dstOffset,
                               jlong size) {
  ScopedFastNativeObjectAccess soa(env);
  if (size == 0) {
    return;
  }
  if (!ValidJniSizeArgument(size)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }
  const size_t memcpy_size = static_cast<size_t>(size);
  const size_t src_offset = static_cast<size_t>(srcOffset);
  ObjPtr<mirror::Object> src = soa.Decode<mirror::Object>(srcObj);
  const size_t dst_offset = static_cast<size_t>(dstOffset);
  ObjPtr<mirror::Object> dst = soa.Decode<mirror::Object>(dstObj);
  memcpy(reinterpret_cast<uint8_t*>(dst.Ptr()) + dst_offset,
         reinterpret_cast<uint8_t*>(src.Ptr()) + src_offset,
         memcpy_size);
}

static jboolean Unsafe_getBoolean(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  return obj->GetFieldBoolean(MemberOffset(offset));
}

static void Unsafe_putBoolean(JNIEnv* env, jobject, jobject javaObj, jlong offset, jboolean newValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  // JNI must use non transactional mode (SetField8 is non-transactional).
  obj->SetFieldBoolean<false>(MemberOffset(offset), newValue);
}

static jbyte Unsafe_getByte(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  return obj->GetFieldByte(MemberOffset(offset));
}

static void Unsafe_putByte(JNIEnv* env, jobject, jobject javaObj, jlong offset, jbyte newValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  // JNI must use non transactional mode.
  obj->SetFieldByte<false>(MemberOffset(offset), newValue);
}

static jchar Unsafe_getChar(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  return obj->GetFieldChar(MemberOffset(offset));
}

static void Unsafe_putChar(JNIEnv* env, jobject, jobject javaObj, jlong offset, jchar newValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  // JNI must use non transactional mode.
  obj->SetFieldChar<false>(MemberOffset(offset), newValue);
}

static jshort Unsafe_getShort(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  return obj->GetFieldShort(MemberOffset(offset));
}

static void Unsafe_putShort(JNIEnv* env, jobject, jobject javaObj, jlong offset, jshort newValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  // JNI must use non transactional mode.
  obj->SetFieldShort<false>(MemberOffset(offset), newValue);
}

static jfloat Unsafe_getFloat(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  union {int32_t val; jfloat converted;} conv;
  conv.val = obj->GetField32(MemberOffset(offset));
  return conv.converted;
}

static void Unsafe_putFloat(JNIEnv* env, jobject, jobject javaObj, jlong offset, jfloat newValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  union {int32_t converted; jfloat val;} conv;
  conv.val = newValue;
  // JNI must use non transactional mode.
  obj->SetField32<false>(MemberOffset(offset), conv.converted);
}

static jdouble Unsafe_getDouble(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  union {int64_t val; jdouble converted;} conv;
  conv.val = obj->GetField64(MemberOffset(offset));
  return conv.converted;
}

static void Unsafe_putDouble(JNIEnv* env, jobject, jobject javaObj, jlong offset, jdouble newValue) {
  ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(javaObj);
  union {int64_t converted; jdouble val;} conv;
  conv.val = newValue;
  // JNI must use non transactional mode.
  obj->SetField64<false>(MemberOffset(offset), conv.converted);
}

static void Unsafe_loadFence(JNIEnv*, jobject) {
  std::atomic_thread_fence(std::memory_order_acquire);
}

static void Unsafe_storeFence(JNIEnv*, jobject) {
  std::atomic_thread_fence(std::memory_order_release);
}

static void Unsafe_fullFence(JNIEnv*, jobject) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

static void Unsafe_park(JNIEnv* env, jobject, jboolean isAbsolute, jlong time) {
  ScopedObjectAccess soa(env);
  Thread::Current()->Park(isAbsolute, time);
}

static void Unsafe_unpark(JNIEnv* env, jobject, jobject jthread) {
  art::ScopedFastNativeObjectAccess soa(env);
  ObjPtr<mirror::Object> mirror_thread = soa.Decode<mirror::Object>(jthread);
  if (mirror_thread == nullptr ||
      !mirror_thread->InstanceOf(WellKnownClasses::java_lang_Thread.Get())) {
    ThrowIllegalArgumentException("Argument to unpark() was not a Thread");
    return;
  }
  Thread* self = soa.Self();
  art::MutexLock mu(self, *art::Locks::thread_list_lock_);
  art::Thread* thread = art::Thread::FromManagedThread(self, mirror_thread);
  if (thread != nullptr) {
    thread->Unpark();
  } else {
    // If thread is null, that means that either the thread is not started yet,
    // or the thread has already terminated. Setting the field to true will be
    // respected when the thread does start, and is harmless if the thread has
    // already terminated.
    ArtField* unparked = WellKnownClasses::java_lang_Thread_unparkedBeforeStart;
    // JNI must use non transactional mode.
    unparked->SetBoolean<false>(mirror_thread, JNI_TRUE);
  }
}

static JNINativeMethod gMethods[] = {
    FAST_NATIVE_METHOD(Unsafe, compareAndSwapInt, "(Ljava/lang/Object;JII)Z"),
    FAST_NATIVE_METHOD(Unsafe, compareAndSwapLong, "(Ljava/lang/Object;JJJ)Z"),
    FAST_NATIVE_METHOD(
        Unsafe, compareAndSwapObject, "(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z"),
    FAST_NATIVE_METHOD(Unsafe, compareAndSetInt, "(Ljava/lang/Object;JII)Z"),
    FAST_NATIVE_METHOD(Unsafe, compareAndSetLong, "(Ljava/lang/Object;JJJ)Z"),
    FAST_NATIVE_METHOD(Unsafe,
                       compareAndSetReference,
                       "(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z"),
    FAST_NATIVE_METHOD(Unsafe, getIntVolatile, "(Ljava/lang/Object;J)I"),
    FAST_NATIVE_METHOD(Unsafe, putIntVolatile, "(Ljava/lang/Object;JI)V"),
    FAST_NATIVE_METHOD(Unsafe, getLongVolatile, "(Ljava/lang/Object;J)J"),
    FAST_NATIVE_METHOD(Unsafe, putLongVolatile, "(Ljava/lang/Object;JJ)V"),
    FAST_NATIVE_METHOD(Unsafe, getReferenceVolatile, "(Ljava/lang/Object;J)Ljava/lang/Object;"),
    FAST_NATIVE_METHOD(Unsafe, putReferenceVolatile, "(Ljava/lang/Object;JLjava/lang/Object;)V"),
    FAST_NATIVE_METHOD(Unsafe, getInt, "(Ljava/lang/Object;J)I"),
    FAST_NATIVE_METHOD(Unsafe, putInt, "(Ljava/lang/Object;JI)V"),
    FAST_NATIVE_METHOD(Unsafe, putOrderedInt, "(Ljava/lang/Object;JI)V"),
    FAST_NATIVE_METHOD(Unsafe, getLong, "(Ljava/lang/Object;J)J"),
    FAST_NATIVE_METHOD(Unsafe, putLong, "(Ljava/lang/Object;JJ)V"),
    FAST_NATIVE_METHOD(Unsafe, putOrderedLong, "(Ljava/lang/Object;JJ)V"),
    FAST_NATIVE_METHOD(Unsafe, getReference, "(Ljava/lang/Object;J)Ljava/lang/Object;"),
    FAST_NATIVE_METHOD(Unsafe, putReference, "(Ljava/lang/Object;JLjava/lang/Object;)V"),
    FAST_NATIVE_METHOD(Unsafe, putOrderedObject, "(Ljava/lang/Object;JLjava/lang/Object;)V"),
    FAST_NATIVE_METHOD(Unsafe, getArrayBaseOffsetForComponentType, "(Ljava/lang/Class;)I"),
    FAST_NATIVE_METHOD(Unsafe, getArrayIndexScaleForComponentType, "(Ljava/lang/Class;)I"),
    FAST_NATIVE_METHOD(Unsafe, addressSize, "()I"),
    FAST_NATIVE_METHOD(Unsafe, pageSize, "()I"),
    FAST_NATIVE_METHOD(Unsafe, allocateMemory, "(J)J"),
    FAST_NATIVE_METHOD(Unsafe, freeMemory, "(J)V"),
    FAST_NATIVE_METHOD(Unsafe, setMemory, "(JJB)V"),
    FAST_NATIVE_METHOD(Unsafe, copyMemory0, "(Ljava/lang/Object;JLjava/lang/Object;JJ)V"),
    FAST_NATIVE_METHOD(Unsafe, getBoolean, "(Ljava/lang/Object;J)Z"),

    FAST_NATIVE_METHOD(Unsafe, getByte, "(Ljava/lang/Object;J)B"),
    FAST_NATIVE_METHOD(Unsafe, getChar, "(Ljava/lang/Object;J)C"),
    FAST_NATIVE_METHOD(Unsafe, getShort, "(Ljava/lang/Object;J)S"),
    FAST_NATIVE_METHOD(Unsafe, getFloat, "(Ljava/lang/Object;J)F"),
    FAST_NATIVE_METHOD(Unsafe, getDouble, "(Ljava/lang/Object;J)D"),
    FAST_NATIVE_METHOD(Unsafe, putBoolean, "(Ljava/lang/Object;JZ)V"),
    FAST_NATIVE_METHOD(Unsafe, putByte, "(Ljava/lang/Object;JB)V"),
    FAST_NATIVE_METHOD(Unsafe, putChar, "(Ljava/lang/Object;JC)V"),
    FAST_NATIVE_METHOD(Unsafe, putShort, "(Ljava/lang/Object;JS)V"),
    FAST_NATIVE_METHOD(Unsafe, putFloat, "(Ljava/lang/Object;JF)V"),
    FAST_NATIVE_METHOD(Unsafe, putDouble, "(Ljava/lang/Object;JD)V"),
    FAST_NATIVE_METHOD(Unsafe, unpark, "(Ljava/lang/Object;)V"),
    NATIVE_METHOD(Unsafe, park, "(ZJ)V"),

    // Each of the getFoo variants are overloaded with a call that operates
    // directively on a native pointer.
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, getByte, "(J)B", getByteJ),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, getChar, "(J)C", getCharJ),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, getShort, "(J)S", getShortJ),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, getInt, "(J)I", getIntJ),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, getLong, "(J)J", getLongJ),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, getFloat, "(J)F", getFloatJ),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, getDouble, "(J)D", getDoubleJ),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, putByte, "(JB)V", putByteJB),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, putChar, "(JC)V", putCharJC),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, putShort, "(JS)V", putShortJS),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, putInt, "(JI)V", putIntJI),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, putLong, "(JJ)V", putLongJJ),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, putFloat, "(JF)V", putFloatJF),
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, putDouble, "(JD)V", putDoubleJD),

    // objectFieldOffset(Class, String) — native override of Java implementation
    // to avoid Field.getName() issues during early class init
    OVERLOADED_FAST_NATIVE_METHOD(Unsafe, objectFieldOffset,
        "(Ljava/lang/Class;Ljava/lang/String;)J", objectFieldOffsetClassString),

    // CAS
    FAST_NATIVE_METHOD(Unsafe, loadFence, "()V"),
    FAST_NATIVE_METHOD(Unsafe, storeFence, "()V"),
    FAST_NATIVE_METHOD(Unsafe, fullFence, "()V"),
};

void register_jdk_internal_misc_Unsafe(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("jdk/internal/misc/Unsafe");
}

}  // namespace art
