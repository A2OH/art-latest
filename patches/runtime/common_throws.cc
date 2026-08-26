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

#include "common_throws.h"
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <iostream>
#include <cstdlib>

#include <atomic>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "art_method.h"
#include "class_linker-inl.h"
#include "debug_print.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_instruction-inl.h"
#include "dex/invoke_type.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/method_type.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "nativehelper/scoped_local_ref.h"
#include "obj_ptr-inl.h"
#include "thread.h"
#include "well_known_classes-inl.h"

namespace art HIDDEN {

using android::base::StringAppendV;
using android::base::StringPrintf;

static void AddReferrerLocation(std::ostream& os, ObjPtr<mirror::Class> referrer)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (referrer != nullptr) {
    std::string location(referrer->GetLocation());
    if (!location.empty()) {
      os << " (declaration of '" << referrer->PrettyDescriptor()
         << "' appears in " << location << ")";
    }
  }
}

static void ThrowException(const char* exception_descriptor) REQUIRES_SHARED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  self->ThrowNewException(exception_descriptor, nullptr);
}

static void ThrowException(const char* exception_descriptor,
                           ObjPtr<mirror::Class> referrer,
                           const char* fmt,
                           va_list* args = nullptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::ostringstream msg;
  if (args != nullptr) {
    std::string vmsg;
    StringAppendV(&vmsg, fmt, *args);
    msg << vmsg;
  } else {
    msg << fmt;
  }
  AddReferrerLocation(msg, referrer);
  Thread* self = Thread::Current();
  self->ThrowNewException(exception_descriptor, msg.str().c_str());
}

static void ThrowWrappedException(const char* exception_descriptor,
                                  ObjPtr<mirror::Class> referrer,
                                  const char* fmt,
                                  va_list* args = nullptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::ostringstream msg;
  if (args != nullptr) {
    std::string vmsg;
    StringAppendV(&vmsg, fmt, *args);
    msg << vmsg;
  } else {
    msg << fmt;
  }
  AddReferrerLocation(msg, referrer);
  Thread* self = Thread::Current();
  self->ThrowNewWrappedException(exception_descriptor, msg.str().c_str());
}

// AbstractMethodError

void ThrowAbstractMethodError(ArtMethod* method) {
  ThrowException("Ljava/lang/AbstractMethodError;", nullptr,
                 StringPrintf("abstract method \"%s\"",
                              ArtMethod::PrettyMethod(method).c_str()).c_str());
}

void ThrowAbstractMethodError(uint32_t method_idx, const DexFile& dex_file) {
  ThrowException("Ljava/lang/AbstractMethodError;", /* referrer= */ nullptr,
                 StringPrintf("abstract method \"%s\"",
                              dex_file.PrettyMethod(method_idx,
                                                    /* with_signature= */ true).c_str()).c_str());
}

// ArithmeticException

void ThrowArithmeticExceptionDivideByZero() {
  ThrowException("Ljava/lang/ArithmeticException;", nullptr, "divide by zero");
}

// ArrayIndexOutOfBoundsException

void ThrowArrayIndexOutOfBoundsException(int index, int length) {
  /* PF-arch-051: dump the Java stack at the throw site to logcat so the agent
   * lifecycle runner can identify which array access fails. ART pre-allocates
   * the exception object for hot paths and our catch site sees an empty stack.
   * Limit the dump to first N firings to avoid log flood. */
  static std::atomic<int> diag_count(0);
  if (diag_count.fetch_add(1) < 8) {
    Thread* self = Thread::Current();
    std::ostringstream os;
    os << "[PF-arch-051] ThrowAIOOB length=" << length << " index=" << index << " stack:\n";
    if (self != nullptr) {
      self->DumpJavaStack(os);
    }
    fprintf(stderr, "%s", os.str().c_str());
    fflush(stderr);
  }
  ThrowException("Ljava/lang/ArrayIndexOutOfBoundsException;", nullptr,
                 StringPrintf("length=%d; index=%d", length, index).c_str());
}

// ArrayStoreException

void ThrowArrayStoreException(ObjPtr<mirror::Class> element_class,
                              ObjPtr<mirror::Class> array_class) {
  // WESTLAKE §603f ASE-DIAG: with the JIT on, this port throws IMPOSSIBLE array-store exceptions
  // ("java.lang.String cannot be stored in an array of type java.lang.String[]"), 3 per launch,
  // never with the JIT off. Storing a String into a String[] is always legal, so the assignability
  // test is answering wrongly. The decisive question is whether element_class and
  // array_class->GetComponentType() are the SAME Class object: if the pointers differ while the
  // descriptors match, there are DUPLICATE Class objects (class-loader / dex-cache identity), not a
  // broken comparison. Dump both pointers plus the Java stack that names the store.
  // Gated on WESTLAKE_ASE_DIAG=1 and capped, so normal runs are unaffected.
  {
    static const bool wl_ase_diag = (getenv("WESTLAKE_ASE_DIAG") != nullptr);
    static int wl_ase_count = 0;
    if (UNLIKELY(wl_ase_diag) && wl_ase_count < 10) {
      wl_ase_count++;
      ObjPtr<mirror::Class> comp =
          (array_class != nullptr) ? array_class->GetComponentType() : nullptr;
      fprintf(stderr,
              "[WESTLAKE-ASE-DIAG] #%d element=%p (%s) array=%p (%s) component=%p (%s) "
              "element==component:%d\n",
              wl_ase_count,
              element_class.Ptr(), mirror::Class::PrettyDescriptor(element_class).c_str(),
              array_class.Ptr(), mirror::Class::PrettyDescriptor(array_class).c_str(),
              comp.Ptr(), mirror::Class::PrettyDescriptor(comp).c_str(),
              (element_class.Ptr() == comp.Ptr()) ? 1 : 0);
      fflush(stderr);
      Thread* self_ase = Thread::Current();
      if (self_ase != nullptr) {
        fprintf(stderr, "[WESTLAKE-ASE-DIAG] #%d Java stack:\n", wl_ase_count);
        fflush(stderr);
        self_ase->DumpJavaStack(std::cerr);
        std::cerr.flush();
      }
    }
  }
  ThrowException("Ljava/lang/ArrayStoreException;", nullptr,
                 StringPrintf("%s cannot be stored in an array of type %s",
                              mirror::Class::PrettyDescriptor(element_class).c_str(),
                              mirror::Class::PrettyDescriptor(array_class).c_str()).c_str());
}

// BootstrapMethodError

void ThrowBootstrapMethodError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/BootstrapMethodError;", nullptr, fmt, &args);
  va_end(args);
}

void ThrowWrappedBootstrapMethodError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowWrappedException("Ljava/lang/BootstrapMethodError;", nullptr, fmt, &args);
  va_end(args);
}

// ClassCastException

void ThrowClassCastException(ObjPtr<mirror::Class> dest_type, ObjPtr<mirror::Class> src_type) {
  ThrowException("Ljava/lang/ClassCastException;", nullptr,
                 StringPrintf("%s cannot be cast to %s",
                              mirror::Class::PrettyDescriptor(src_type).c_str(),
                              mirror::Class::PrettyDescriptor(dest_type).c_str()).c_str());
}

void ThrowClassCastException(const char* msg) {
  ThrowException("Ljava/lang/ClassCastException;", nullptr, msg);
}

// ClassCircularityError

void ThrowClassCircularityError(ObjPtr<mirror::Class> c) {
  std::ostringstream msg;
  msg << mirror::Class::PrettyDescriptor(c);
  ThrowException("Ljava/lang/ClassCircularityError;", c, msg.str().c_str());
}

void ThrowClassCircularityError(ObjPtr<mirror::Class> c, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/ClassCircularityError;", c, fmt, &args);
  va_end(args);
}

// ClassFormatError

void ThrowClassFormatError(ObjPtr<mirror::Class> referrer, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/ClassFormatError;", referrer, fmt, &args);
  va_end(args);
}

// IllegalAccessError

void ThrowIllegalAccessErrorClass(ObjPtr<mirror::Class> referrer, ObjPtr<mirror::Class> accessed) {
  std::ostringstream msg;
  msg << "Illegal class access: '" << mirror::Class::PrettyDescriptor(referrer)
      << "' attempting to access '" << mirror::Class::PrettyDescriptor(accessed) << "'";
  ThrowException("Ljava/lang/IllegalAccessError;", referrer, msg.str().c_str());
}

void ThrowIllegalAccessErrorClassForMethodDispatch(ObjPtr<mirror::Class> referrer,
                                                   ObjPtr<mirror::Class> accessed,
                                                   ArtMethod* called,
                                                   InvokeType type) {
  std::ostringstream msg;
  msg << "Illegal class access ('" << mirror::Class::PrettyDescriptor(referrer)
      << "' attempting to access '"
      << mirror::Class::PrettyDescriptor(accessed) << "') in attempt to invoke " << type
      << " method " << ArtMethod::PrettyMethod(called).c_str();
  ThrowException("Ljava/lang/IllegalAccessError;", referrer, msg.str().c_str());
}

void ThrowIllegalAccessErrorMethod(ObjPtr<mirror::Class> referrer, ArtMethod* accessed) {
  std::ostringstream msg;
  msg << "Method '" << ArtMethod::PrettyMethod(accessed) << "' is inaccessible to class '"
      << mirror::Class::PrettyDescriptor(referrer) << "'";
  ThrowException("Ljava/lang/IllegalAccessError;", referrer, msg.str().c_str());
}

void ThrowIllegalAccessErrorField(ObjPtr<mirror::Class> referrer, ArtField* accessed) {
  std::ostringstream msg;
  msg << "Field '" << ArtField::PrettyField(accessed, false) << "' is inaccessible to class '"
      << mirror::Class::PrettyDescriptor(referrer) << "'";
  ThrowException("Ljava/lang/IllegalAccessError;", referrer, msg.str().c_str());
}

void ThrowIllegalAccessErrorFinalField(ArtMethod* referrer, ArtField* accessed) {
  std::ostringstream msg;
  msg << "Final field '" << ArtField::PrettyField(accessed, false)
      << "' cannot be written to by method '" << ArtMethod::PrettyMethod(referrer) << "'";
  ThrowException("Ljava/lang/IllegalAccessError;",
                 referrer != nullptr ? referrer->GetDeclaringClass() : nullptr,
                 msg.str().c_str());
}

void ThrowIllegalAccessError(ObjPtr<mirror::Class> referrer, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/IllegalAccessError;", referrer, fmt, &args);
  va_end(args);
}

void ThrowIllegalAccessErrorForImplementingMethod(ObjPtr<mirror::Class> klass,
                                                  ArtMethod* implementation_method,
                                                  ArtMethod* interface_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Note: For a non-public abstract implementing method, both `AbstractMethodError` and
  // `IllegalAccessError` are reasonable. We now follow the RI behaviour and throw the latter,
  // so we do not assert here that the implementation method is concrete as we did in the past.
  DCHECK(!implementation_method->IsPublic());
  ThrowIllegalAccessError(
      klass,
      "Method '%s' implementing interface method '%s' is not public",
      implementation_method->PrettyMethod().c_str(),
      interface_method->PrettyMethod().c_str());
}

// IllegalAccessException

void ThrowIllegalAccessException(const char* msg) {
  ThrowException("Ljava/lang/IllegalAccessException;", nullptr, msg);
}

// IllegalArgumentException

void ThrowIllegalArgumentException(const char* msg) {
  ThrowException("Ljava/lang/IllegalArgumentException;", nullptr, msg);
}

// IllegalStateException

void ThrowIllegalStateException(const char* msg) {
  ThrowException("Ljava/lang/IllegalStateException;", nullptr, msg);
}

// IncompatibleClassChangeError

void ThrowIncompatibleClassChangeError(InvokeType expected_type,
                                       InvokeType found_type,
                                       ArtMethod* method,
                                       ArtMethod* referrer) {
  std::ostringstream msg;
  msg << "The method '" << ArtMethod::PrettyMethod(method) << "' was expected to be of type "
      << expected_type << " but instead was found to be of type " << found_type;
  ThrowException("Ljava/lang/IncompatibleClassChangeError;",
                 referrer != nullptr ? referrer->GetDeclaringClass() : nullptr,
                 msg.str().c_str());
}

void ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(ArtMethod* interface_method,
                                                                ObjPtr<mirror::Object> this_object,
                                                                ArtMethod* referrer) {
  // Referrer is calling interface_method on this_object, however, the interface_method isn't
  // implemented by this_object.
  CHECK(this_object != nullptr);
  std::ostringstream msg;
  msg << "Class '" << mirror::Class::PrettyDescriptor(this_object->GetClass())
      << "' does not implement interface '"
      << mirror::Class::PrettyDescriptor(interface_method->GetDeclaringClass())
      << "' in call to '" << ArtMethod::PrettyMethod(interface_method) << "'";
  ThrowException("Ljava/lang/IncompatibleClassChangeError;",
                 referrer != nullptr ? referrer->GetDeclaringClass() : nullptr,
                 msg.str().c_str());
}

void ThrowIncompatibleClassChangeErrorField(ArtField* resolved_field,
                                            bool is_static,
                                            ArtMethod* referrer) {
  std::ostringstream msg;
  msg << "Expected '" << ArtField::PrettyField(resolved_field) << "' to be a "
      << (is_static ? "static" : "instance") << " field" << " rather than a "
      << (is_static ? "instance" : "static") << " field";
  ThrowException("Ljava/lang/IncompatibleClassChangeError;", referrer->GetDeclaringClass(),
                 msg.str().c_str());
}

void ThrowIncompatibleClassChangeError(ObjPtr<mirror::Class> referrer, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/IncompatibleClassChangeError;", referrer, fmt, &args);
  va_end(args);
}

void ThrowIncompatibleClassChangeErrorForMethodConflict(ArtMethod* method) {
  DCHECK(method != nullptr);
  ThrowException("Ljava/lang/IncompatibleClassChangeError;",
                 /*referrer=*/nullptr,
                 StringPrintf("Conflicting default method implementations %s",
                              ArtMethod::PrettyMethod(method).c_str()).c_str());
}

// IndexOutOfBoundsException

void ThrowIndexOutOfBoundsException(int index, int length) {
  ThrowException("Ljava/lang/IndexOutOfBoundsException;", nullptr,
                 StringPrintf("length=%d; index=%d", length, index).c_str());
}

// InternalError

void ThrowInternalError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/InternalError;", nullptr, fmt, &args);
  va_end(args);
}

// IOException

void ThrowIOException(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/io/IOException;", nullptr, fmt, &args);
  va_end(args);
}

void ThrowWrappedIOException(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowWrappedException("Ljava/io/IOException;", nullptr, fmt, &args);
  va_end(args);
}

// LinkageError

void ThrowLinkageError(ObjPtr<mirror::Class> referrer, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/LinkageError;", referrer, fmt, &args);
  va_end(args);
}

void ThrowWrappedLinkageError(ObjPtr<mirror::Class> referrer, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowWrappedException("Ljava/lang/LinkageError;", referrer, fmt, &args);
  va_end(args);
}

// NegativeArraySizeException

void ThrowNegativeArraySizeException(int size) {
  ThrowException("Ljava/lang/NegativeArraySizeException;", nullptr,
                 StringPrintf("%d", size).c_str());
}

void ThrowNegativeArraySizeException(const char* msg) {
  ThrowException("Ljava/lang/NegativeArraySizeException;", nullptr, msg);
}

// NoSuchFieldError

void ThrowNoSuchFieldError(std::string_view scope,
                           ObjPtr<mirror::Class> c,
                           std::string_view type,
                           std::string_view name) {
  std::ostringstream msg;
  std::string temp;
  msg << "No " << scope << "field " << name << " of type " << type
      << " in class " << c->GetDescriptor(&temp) << " or its superclasses";
  ThrowException("Ljava/lang/NoSuchFieldError;", c, msg.str().c_str());
}

void ThrowNoSuchFieldException(ObjPtr<mirror::Class> c, std::string_view name) {
  std::ostringstream msg;
  std::string temp;
  msg << "No field " << name << " in class " << c->GetDescriptor(&temp);
  ThrowException("Ljava/lang/NoSuchFieldException;", c, msg.str().c_str());
}

// NoSuchMethodError

void ThrowNoSuchMethodError(InvokeType type,
                            ObjPtr<mirror::Class> c,
                            std::string_view name,
                            const Signature& signature) {
  std::ostringstream msg;
  std::string temp;
  msg << "No " << type << " method " << name << signature
      << " in class " << c->GetDescriptor(&temp) << " or its super classes";
  ThrowException("Ljava/lang/NoSuchMethodError;", c, msg.str().c_str());
}

// NullPointerException

void ThrowNullPointerExceptionForFieldAccess(ArtField* field, ArtMethod* method, bool is_read) {
  std::ostringstream msg;
  msg << "Attempt to " << (is_read ? "read from" : "write to") << " field '"
      << ArtField::PrettyField(field) << "' on a null object reference in method '"
      << ArtMethod::PrettyMethod(method) << "'";
  ThrowException("Ljava/lang/NullPointerException;", nullptr, msg.str().c_str());
}

static void ThrowNullPointerExceptionForMethodAccessImpl(uint32_t method_idx,
                                                         const DexFile& dex_file,
                                                         InvokeType type)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // WESTLAKE (arm64 board, 2026-07-21) NPE-DIAG: ActivityThread.handleBindApplication dies
  // with "Attempt to invoke ... String.contains(CharSequence) on a null object reference"
  // and the adapter only logs "Caused by:" with no frames, so dump the Java stack at the
  // throw site.  Gated on WESTLAKE_NPE_DIAG so normal runs are unaffected.
  // NOT env-gated: the spawned child resets `environ`, so getenv() is useless there
  // (same lesson as the libart THROW-DIAG). Rate-limit instead.
  {
    static int westlake_npe_diag_count = 0;
    if (westlake_npe_diag_count < 3) {
      westlake_npe_diag_count++;
      Thread* self_diag = Thread::Current();
      // WESTLAKE 2026-07-22 (§178): when Thread::Current() is NULL the message below is still
      // built and thrown but NO stack is dumped -- that is exactly how the failing
      // AtomicReferenceFieldUpdater.compareAndSet NPE escaped every diagnostic. Report the null
      // case explicitly with OS thread ids so the offending (unattached) thread can be identified.
      if (self_diag == nullptr) {
        fprintf(stderr,
                "[WESTLAKE-NOTHREAD] NPE thrown with Thread::Current()==NULL "
                "tid=%d pthread=%p type=%d method_idx=%u\n",
                static_cast<int>(syscall(SYS_gettid)),
                reinterpret_cast<void*>(pthread_self()),
                static_cast<int>(type), method_idx);
        fflush(stderr);
      }
      if (self_diag != nullptr) {
        fprintf(stderr, "[WESTLAKE-NPE-DIAG] NPE on invoke #%d, Java stack:\n",
                westlake_npe_diag_count);
        fflush(stderr);
        self_diag->DumpJavaStack(std::cerr);
        std::cerr.flush();
      }
    }
  }
  {
    static int wl_t = 0;
    if (wl_t < 20) { wl_t++;
      fprintf(stderr, "[WESTLAKE-INVTYPE] #%d raw_type=%d method_idx=%u\n",
              wl_t, static_cast<int>(type), method_idx);
      fflush(stderr); }
  }
  std::ostringstream msg;
  msg << "Attempt to invoke " << type << " method '"
      << dex_file.PrettyMethod(method_idx, true) << "' on a null object reference";
  // PATCH: Log caller + stack for NPE debugging
  {
    Thread* self = Thread::Current();
    // Walk shadow frames to get the actual caller chain
    fprintf(stderr, "[NPE] %s\n", dex_file.PrettyMethod(method_idx, true).c_str());
    int depth = 0;
    for (auto* frame = self->GetManagedStack()->GetTopShadowFrame();
         frame != nullptr && depth < 8;
         frame = frame->GetLink(), depth++) {
      ArtMethod* m = frame->GetMethod();
      if (m != nullptr) {
        fprintf(stderr, "[NPE]   #%d %s (dex_pc=%u)\n", depth, m->PrettyMethod().c_str(),
                frame->GetDexPC());
      }
    }
    fflush(stderr);
  }
  ThrowException("Ljava/lang/NullPointerException;", nullptr, msg.str().c_str());
}

void ThrowNullPointerExceptionForMethodAccess(uint32_t method_idx, InvokeType type) {
  const DexFile& dex_file = *Thread::Current()->GetCurrentMethod(nullptr)->GetDexFile();
  ThrowNullPointerExceptionForMethodAccessImpl(method_idx, dex_file, type);
}

void ThrowNullPointerExceptionForMethodAccess(ArtMethod* method, InvokeType type) {
  ThrowNullPointerExceptionForMethodAccessImpl(method->GetDexMethodIndex(),
                                               *method->GetDexFile(),
                                               type);
}

static bool IsValidReadBarrierImplicitCheck(uintptr_t addr) {
  DCHECK(gUseReadBarrier);
  uint32_t monitor_offset = mirror::Object::MonitorOffset().Uint32Value();
  if (kUseBakerReadBarrier &&
      (kRuntimeISA == InstructionSet::kX86 || kRuntimeISA == InstructionSet::kX86_64)) {
    constexpr uint32_t gray_byte_position = LockWord::kReadBarrierStateShift / kBitsPerByte;
    monitor_offset += gray_byte_position;
  }
  return addr == monitor_offset;
}

static bool IsValidImplicitCheck(uintptr_t addr, const Instruction& instr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!CanDoImplicitNullCheckOn(addr)) {
    return false;
  }

  switch (instr.Opcode()) {
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
    case Instruction::INVOKE_POLYMORPHIC:
    case Instruction::INVOKE_POLYMORPHIC_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE: {
      // Without inlining, we could just check that the offset is the class offset.
      // However, when inlining, the compiler can (validly) merge the null check with a field access
      // on the same object. Note that the stack map at the NPE will reflect the invoke's location,
      // which is the caller.
      return true;
    }

    case Instruction::IGET_OBJECT:
      if (gUseReadBarrier && IsValidReadBarrierImplicitCheck(addr)) {
        return true;
      }
      FALLTHROUGH_INTENDED;
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT:
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT: {
      // We might be doing an implicit null check with an offset that doesn't correspond
      // to the instruction, for example with two field accesses and the first one being
      // eliminated or re-ordered.
      return true;
    }

    case Instruction::AGET_OBJECT:
      if (gUseReadBarrier && IsValidReadBarrierImplicitCheck(addr)) {
        return true;
      }
      FALLTHROUGH_INTENDED;
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_OBJECT:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_CHAR:
    case Instruction::APUT_SHORT:
    case Instruction::FILL_ARRAY_DATA:
    case Instruction::ARRAY_LENGTH: {
      // The length access should crash. We currently do not do implicit checks on
      // the array access itself.
      return (addr == 0u) || (addr == mirror::Array::LengthOffset().Uint32Value());
    }

    default: {
      // We have covered all the cases where an NPE could occur.
      // Note that this must be kept in sync with the compiler, and adding
      // any new way to do implicit checks in the compiler should also update
      // this code.
      return false;
    }
  }
}

void ThrowNullPointerExceptionFromDexPC(bool check_address, uintptr_t addr) {
  uint32_t throw_dex_pc;
  ArtMethod* method = Thread::Current()->GetCurrentMethod(&throw_dex_pc);
  CodeItemInstructionAccessor accessor(method->DexInstructions());
  CHECK_LT(throw_dex_pc, accessor.InsnsSizeInCodeUnits());
  const Instruction& instr = accessor.InstructionAt(throw_dex_pc);
  if (check_address && !IsValidImplicitCheck(addr, instr)) {
    const DexFile* dex_file = method->GetDexFile();
    LOG(FATAL) << "Invalid address for an implicit NullPointerException check: "
               << "0x" << std::hex << addr << std::dec
               << ", at "
               << instr.DumpString(dex_file)
               << " in "
               << method->PrettyMethod();
  }

  switch (instr.Opcode()) {
    case Instruction::INVOKE_DIRECT:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_35c(), kDirect);
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_3rc(), kDirect);
      break;
    case Instruction::INVOKE_VIRTUAL:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_35c(), kVirtual);
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_3rc(), kVirtual);
      break;
    case Instruction::INVOKE_SUPER:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_35c(), kSuper);
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_3rc(), kSuper);
      break;
    case Instruction::INVOKE_INTERFACE:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_35c(), kInterface);
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_3rc(), kInterface);
      break;
    case Instruction::INVOKE_POLYMORPHIC:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_45cc(), kVirtual);
      break;
    case Instruction::INVOKE_POLYMORPHIC_RANGE:
      ThrowNullPointerExceptionForMethodAccess(instr.VRegB_4rcc(), kVirtual);
      break;
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT: {
      ArtField* field =
          Runtime::Current()->GetClassLinker()->ResolveField(instr.VRegC_22c(), method, false);
      Thread::Current()->ClearException();  // Resolution may fail, ignore.
      ThrowNullPointerExceptionForFieldAccess(field, method, /* is_read= */ true);
      break;
    }
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT: {
      ArtField* field = Runtime::Current()->GetClassLinker()->ResolveField(
          instr.VRegC_22c(), method, /* is_static= */ false);
      Thread::Current()->ClearException();  // Resolution may fail, ignore.
      ThrowNullPointerExceptionForFieldAccess(field, method, /* is_read= */ false);
      break;
    }
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_OBJECT:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
      ThrowException("Ljava/lang/NullPointerException;", nullptr,
                     "Attempt to read from null array");
      break;
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_OBJECT:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_CHAR:
    case Instruction::APUT_SHORT:
      ThrowException("Ljava/lang/NullPointerException;", nullptr,
                     "Attempt to write to null array");
      break;
    case Instruction::ARRAY_LENGTH:
      {
        // WESTLAKE §434: DISABLED (condition forced false). This diagnostic is ABSENT from
        // the known-good libart — proved by diffing the two binaries' PFCUT/WESTLAKE marker
        // strings — and walking shadow frames + PrettyMethod() on every null-array NPE
        // SIGSEGVs the child during startup. Flip to `< 80` only for targeted debugging.
        // WESTLAKE §611: re-enabled behind WESTLAKE_NULLARR_DIAG (default off; §434 hazard
        // stays dormant). Frame walk additionally gated on WESTLAKE_NULLARR_WALK.
        // WESTLAKE §657 (2026-08-15): the getenv() gates NEVER OPEN — environment variables do
        // not reach appspawn-x children (verified: exported in run_*.sh, 0 hits in the child's
        // /proc/<pid>/environ). That is why this diagnostic reported 0 for the Toutiao
        // "Attempt to get length of null array" NPE that blocks Application.onCreate.
        // Use a FILE trigger instead — the child can read /data/local/tmp/asx even though it
        // cannot write there (it runs as an app uid). Create it before launching:
        //     touch /data/local/tmp/asx/NULLARR        (and .../NULLARR_WALK for frame walking)
        // Checked once per thread, so the cost is one access() per thread that ever throws this.
        static thread_local int pfc_null_array_count = 0;
        static thread_local int wl_nullarr_state = -1;   // -1 unknown, 0 off, 1 on
        if (wl_nullarr_state < 0) {
          wl_nullarr_state = (access("/data/local/tmp/asx/NULLARR", F_OK) == 0) ? 1 : 0;
        }
        const bool wl_nullarr_diag = (wl_nullarr_state == 1);
        if (wl_nullarr_diag && pfc_null_array_count < 40) {
          pfc_null_array_count++;
          Thread* self = Thread::Current();
          fprintf(stderr,
                  "[PFCUT-NPE-ARRAY-LENGTH] method=%s dex_pc=%u shadow=%p quick=%p\n",
                  method != nullptr ? method->PrettyMethod().c_str() : "<null>",
                  throw_dex_pc,
                  self != nullptr && self->GetManagedStack() != nullptr
                      ? self->GetManagedStack()->GetTopShadowFrame()
                      : nullptr,
                  self != nullptr && self->GetManagedStack() != nullptr
                      ? self->GetManagedStack()->GetTopQuickFrame()
                      : nullptr);
          if (access("/data/local/tmp/asx/NULLARR_WALK", F_OK) == 0 &&
              self != nullptr && self->GetManagedStack() != nullptr) {
            int depth = 0;
            for (auto* frame = self->GetManagedStack()->GetTopShadowFrame();
                 frame != nullptr && depth < 16;
                 frame = frame->GetLink(), depth++) {
              ArtMethod* m = frame->GetMethod();
              fprintf(stderr,
                      "[PFCUT-NPE-ARRAY-LENGTH]   #%d %s\n",
                      depth,
                      m != nullptr ? m->PrettyMethod().c_str() : "<null>");
            }
          }
          fflush(stderr);
        }
      }
      ThrowException("Ljava/lang/NullPointerException;", nullptr,
                     "Attempt to get length of null array");
      break;
    case Instruction::FILL_ARRAY_DATA: {
      ThrowException("Ljava/lang/NullPointerException;", nullptr,
                     "Attempt to write to null array");
      break;
    }
    case Instruction::MONITOR_ENTER:
    case Instruction::MONITOR_EXIT: {
      // PATCH: Log the method + shadow frame for sync-on-null debugging
      {
        Thread* self = Thread::Current();
        fprintf(stderr, "[NPE-SYNC] synchronized(null) in %s\n", method->PrettyMethod().c_str());
        int depth = 0;
        for (auto* frame = self->GetManagedStack()->GetTopShadowFrame();
             frame != nullptr && depth < 8;
             frame = frame->GetLink(), depth++) {
          ArtMethod* m = frame->GetMethod();
          if (m != nullptr) {
            fprintf(stderr, "[NPE-SYNC]   #%d %s (dex_pc=%u)\n", depth, m->PrettyMethod().c_str(),
                    frame->GetDexPC());
          }
        }
        fflush(stderr);
      }
      ThrowException("Ljava/lang/NullPointerException;", nullptr,
                     "Attempt to do a synchronize operation on a null object");
      break;
    }
    default: {
      const DexFile* dex_file = method->GetDexFile();
      LOG(FATAL) << "NullPointerException at an unexpected instruction: "
                 << instr.DumpString(dex_file)
                 << " in "
                 << method->PrettyMethod();
      UNREACHABLE();
    }
  }
}

void ThrowNullPointerException(const char* msg) {
  ThrowException("Ljava/lang/NullPointerException;", nullptr, msg);
}

void ThrowNullPointerException() {
  ThrowException("Ljava/lang/NullPointerException;");
}

// ReadOnlyBufferException

void ThrowReadOnlyBufferException() {
  Thread::Current()->ThrowNewException("Ljava/nio/ReadOnlyBufferException;", nullptr);
}

// RuntimeException

void ThrowRuntimeException(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/RuntimeException;", nullptr, fmt, &args);
  va_end(args);
}

// SecurityException

void ThrowSecurityException(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/SecurityException;", nullptr, fmt, &args);
  va_end(args);
}

// Stack overflow.

void ThrowStackOverflowError(Thread* self) {
  if (self->IsHandlingStackOverflow()) {
    LOG(ERROR) << "Recursive stack overflow.";
    // We don't fail here because SetStackEndForStackOverflow will print better diagnostics.
  }

  self->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute.

  // Avoid running Java code for exception initialization.
  // TODO: Checks to make this a bit less brittle.
  //
  // Note: This lambda is used to make sure the `StackOverflowError` intitialization code
  //       does not increase the frame size of `ThrowStackOverflowError()` itself. It runs
  //       with its own frame in the extended stack, which is especially important for modes
  //       with larger stack sizes (e.g., ASAN).
  auto create_and_throw = [self]() REQUIRES_SHARED(Locks::mutator_lock_) NO_INLINE {
    std::string msg("stack size ");
    msg += PrettySize(self->GetStackSize());

    ScopedObjectAccessUnchecked soa(self);
    StackHandleScope<1u> hs(self);

    // Allocate an uninitialized object.
    DCHECK(WellKnownClasses::java_lang_StackOverflowError->IsInitialized());
    Handle<mirror::Object> exc = hs.NewHandle(
        WellKnownClasses::java_lang_StackOverflowError->AllocObject(self));
    if (exc == nullptr) {
      LOG(WARNING) << "Could not allocate StackOverflowError object.";
      return;
    }

    // "Initialize".
    // StackOverflowError -> VirtualMachineError -> Error -> Throwable -> Object.
    // Only Throwable has "custom" fields:
    //   String detailMessage.
    //   Throwable cause (= this).
    //   List<Throwable> suppressedExceptions (= Collections.emptyList()).
    //   Object stackState;
    //   StackTraceElement[] stackTrace;
    // Only Throwable has a non-empty constructor:
    //   this.stackTrace = EmptyArray.STACK_TRACE_ELEMENT;
    //   fillInStackTrace();

    // detailMessage.
    {
      ObjPtr<mirror::String> s = mirror::String::AllocFromModifiedUtf8(self, msg.c_str());
      if (s == nullptr) {
        LOG(WARNING) << "Could not throw new StackOverflowError because message allocation failed.";
        return;
      }
      WellKnownClasses::java_lang_Throwable_detailMessage
          ->SetObject</*kTransactionActive=*/ false>(exc.Get(), s);
    }

    // cause.
    WellKnownClasses::java_lang_Throwable_cause
        ->SetObject</*kTransactionActive=*/ false>(exc.Get(), exc.Get());

    // suppressedExceptions.
    {
      ObjPtr<mirror::Class> j_u_c = WellKnownClasses::java_util_Collections.Get();
      DCHECK(j_u_c->IsInitialized());
      ObjPtr<mirror::Object> empty_list =
          WellKnownClasses::java_util_Collections_EMPTY_LIST->GetObject(j_u_c);
      CHECK(empty_list != nullptr);
      WellKnownClasses::java_lang_Throwable_suppressedExceptions
          ->SetObject</*kTransactionActive=*/ false>(exc.Get(), empty_list);
    }

    // stackState is set as result of fillInStackTrace. fillInStackTrace calls
    // nativeFillInStackTrace.
    ObjPtr<mirror::Object> stack_state_val = self->CreateInternalStackTrace(soa);
    if (stack_state_val != nullptr) {
      WellKnownClasses::java_lang_Throwable_stackState
          ->SetObject</*kTransactionActive=*/ false>(exc.Get(), stack_state_val);

      // stackTrace.
      ObjPtr<mirror::Class> l_u_ea = WellKnownClasses::libcore_util_EmptyArray.Get();
      DCHECK(l_u_ea->IsInitialized());
      ObjPtr<mirror::Object> empty_ste =
          WellKnownClasses::libcore_util_EmptyArray_STACK_TRACE_ELEMENT->GetObject(l_u_ea);
      CHECK(empty_ste != nullptr);
      WellKnownClasses::java_lang_Throwable_stackTrace
          ->SetObject</*kTransactionActive=*/ false>(exc.Get(), empty_ste);
    } else {
      LOG(WARNING) << "Could not create stack trace.";
      // Note: we'll create an exception without stack state, which is valid.
    }

    // Throw the exception.
    self->SetException(exc->AsThrowable());
  };
  create_and_throw();
  CHECK(self->IsExceptionPending());

  self->ResetDefaultStackEnd();  // Return to default stack size.

  // And restore protection if implicit checks are on.
  if (Runtime::Current()->GetImplicitStackOverflowChecks()) {
    self->ProtectStack();
  }
}

// StringIndexOutOfBoundsException

void ThrowStringIndexOutOfBoundsException(int index, int length) {
  ThrowException("Ljava/lang/StringIndexOutOfBoundsException;", nullptr,
                 StringPrintf("length=%d; index=%d", length, index).c_str());
}

// UnsupportedOperationException

void ThrowUnsupportedOperationException() {
  ThrowException("Ljava/lang/UnsupportedOperationException;");
}

// VerifyError

void ThrowVerifyError(ObjPtr<mirror::Class> referrer, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowException("Ljava/lang/VerifyError;", referrer, fmt, &args);
  va_end(args);
}

// WrongMethodTypeException

void ThrowWrongMethodTypeException(ObjPtr<mirror::MethodType> expected_type,
                                   ObjPtr<mirror::MethodType> actual_type) {
  ThrowWrongMethodTypeException(expected_type->PrettyDescriptor(), actual_type->PrettyDescriptor());
}

void ThrowWrongMethodTypeException(const std::string& expected_descriptor,
                                   const std::string& actual_descriptor) {
  std::ostringstream msg;
  msg << "Expected " << expected_descriptor << " but was " << actual_descriptor;
  ThrowException("Ljava/lang/invoke/WrongMethodTypeException;",  nullptr, msg.str().c_str());
}

}  // namespace art
