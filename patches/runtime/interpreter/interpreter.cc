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

#include "interpreter.h"

#include <limits>
#include <string_view>



#include "common_dex_operations.h"
#include "common_throws.h"
#include "dex/dex_file_types.h"
#include "interpreter_common.h"
#include "interpreter_switch_impl.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jvalue-inl.h"
#include "mirror/string-inl.h"
#include "nativehelper/scoped_local_ref.h"
#include "scoped_thread_state_change-inl.h"
#include "shadow_frame-inl.h"
#include "stack.h"
#include "thread-inl.h"
#include "unstarted_runtime.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "jni/java_vm_ext.h"
#include "jni/jni_env_ext.h"

namespace art HIDDEN {
namespace interpreter {

ALWAYS_INLINE static ObjPtr<mirror::Object> ObjArg(uint32_t arg)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return reinterpret_cast<mirror::Object*>(arg);
}

static void InterpreterJni(Thread* self,
                           ArtMethod* method,
                           std::string_view shorty,
                           ObjPtr<mirror::Object> receiver,
                           uint32_t* args,
                           JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Resolve the native function if it hasn't been registered yet.
  // The JNI entry point may be the dlsym lookup stub (an assembly routine),
  // which can't be called as a C function. We must resolve to the actual native.
  {
    const void* jni_entry = method->GetEntryPointFromJni();
    const void* dlsym_stub = GetJniDlsymLookupStub();
    const void* dlsym_critical_stub = GetJniDlsymLookupCriticalStub();
    if (jni_entry == dlsym_stub || jni_entry == dlsym_critical_stub || jni_entry == nullptr) {
      // Need to resolve the native method via JNI name lookup
      JavaVMExt* vm = down_cast<JNIEnvExt*>(self->GetJniEnv())->GetVm();
      std::string error_msg;
      const void* native_code = vm->FindCodeForNativeMethod(method, &error_msg, /*can_suspend=*/true);
      if (native_code == nullptr) {
        // Native method not found — throw UnsatisfiedLinkError
        self->ThrowNewException("Ljava/lang/UnsatisfiedLinkError;", error_msg.c_str());
        return;
      }
      // Register the resolved native code
      Runtime::Current()->GetClassLinker()->RegisterNative(self, method, native_code);
    }
  }

  // @CriticalNative dispatch: raw C call, no JNIEnv/jclass
  // Check: method has @CriticalNative flag AND was resolved from libandroid_runtime
  // (not our manual patches which use JNI calling convention)
  if (method->IsCriticalNative()) {
    // Only use CriticalNative if the function was registered from an external .so
    // (libandroid_runtime.so). Our manual patches use JNI calling convention.
    // Heuristic: check if the name starts with "android.os.Parcel.native" or similar
    // known @CriticalNative methods from the framework.
    // Only dispatch as @CriticalNative for methods on android.os.Parcel
    // (the only class where we know the registration used @CriticalNative
    // calling convention from libandroid_runtime.so)
    std::string cls_desc;
    const char* desc = method->GetDeclaringClass()->GetDescriptor(&cls_desc);
    bool isParcelCritical = (desc != nullptr &&
        (strcmp(desc, "Landroid/os/Parcel;") == 0 ||
         strcmp(desc, "Landroid/graphics/Canvas;") == 0 ||
         strstr(desc, "android/graphics/") != nullptr ||
         strcmp(desc, "Landroid/view/Surface;") == 0 ||
         strcmp(desc, "Landroid/graphics/RecordingCanvas;") == 0));
    if (!isParcelCritical) {
      // Not a known @CriticalNative — use regular JNI dispatch
      goto regular_jni;
    }
    const void* fn = method->GetEntryPointFromJni();
    // @CriticalNative shorty patterns — direct C call with raw args
    if (shorty == "IJ") {
      result->SetI(reinterpret_cast<jint(*)(jlong)>(const_cast<void*>(fn))(
          *reinterpret_cast<jlong*>(&args[0])));
    } else if (shorty == "LJ") {
      ScopedObjectAccessUnchecked soa(self);
      jobject r = reinterpret_cast<jobject(*)(jlong)>(const_cast<void*>(fn))(
          *reinterpret_cast<jlong*>(&args[0]));
      result->SetL(soa.Decode<mirror::Object>(r));
    } else if (shorty == "JJ") {
      result->SetJ(reinterpret_cast<jlong(*)(jlong)>(const_cast<void*>(fn))(
          *reinterpret_cast<jlong*>(&args[0])));
    } else if (shorty == "VJ") {
      reinterpret_cast<void(*)(jlong)>(const_cast<void*>(fn))(
          *reinterpret_cast<jlong*>(&args[0]));
    } else if (shorty == "VJII") {
      // void fn(long, int, int) — Canvas.nativeDrawColor(ptr, color, blendMode)
      reinterpret_cast<void(*)(jlong, jint, jint)>(const_cast<void*>(fn))(
          *reinterpret_cast<jlong*>(&args[0]), args[2], args[3]);
    } else if (shorty == "VJI") {
      reinterpret_cast<void(*)(jlong, jint)>(const_cast<void*>(fn))(
          *reinterpret_cast<jlong*>(&args[0]), args[2]);
    } else if (shorty == "VJL") {
      ScopedObjectAccessUnchecked soa(self);
      ScopedLocalRef<jobject> a1(soa.Env(), soa.AddLocalReference<jobject>(
          reinterpret_cast<StackReference<mirror::Object>*>(&args[2])->AsMirrorPtr()));
      reinterpret_cast<void(*)(jlong, jobject)>(const_cast<void*>(fn))(
          *reinterpret_cast<jlong*>(&args[0]), a1.get());
    } else if (shorty == "VJJ") {
      reinterpret_cast<void(*)(jlong, jlong)>(const_cast<void*>(fn))(
          *reinterpret_cast<jlong*>(&args[0]), *reinterpret_cast<jlong*>(&args[2]));
    } else if (shorty == "J") {
      result->SetJ(reinterpret_cast<jlong(*)()>(const_cast<void*>(fn))());
    } else {
      LOG(WARNING) << "InterpreterJni: unhandled @CriticalNative shorty '" << shorty
                   << "' for " << method->PrettyMethod();
    }
    return;
  }

  // Regular JNI dispatch (JNIEnv + jclass/jobject)
  regular_jni:
  ScopedObjectAccessUnchecked soa(self);
  if (method->IsStatic()) {
    if (shorty == "L") {
      using fntype = jobject(JNIEnv*, jclass);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jobject jresult;
      {
        // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
        jresult = fn(soa.Env(), klass.get());
      }
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "V") {
      using fntype = void(JNIEnv*, jclass);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), klass.get());
    } else if (shorty == "VI") {
      using fntype = void(JNIEnv*, jclass, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), klass.get(), args[0]);
    } else if (shorty == "J") {
      using fntype = jlong(JNIEnv*, jclass);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetJ(fn(soa.Env(), klass.get()));
    } else if (shorty == "ZI") {
      using fntype = jboolean(JNIEnv*, jclass, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetZ(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "JL") {
      using fntype = jlong(JNIEnv*, jclass, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
          soa.AddLocalReference<jobject>(reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetJ(fn(soa.Env(), klass.get(), arg0.get()));
    } else if (shorty == "VLL") {
      using fntype = void(JNIEnv*, jclass, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
          soa.AddLocalReference<jobject>(reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
          soa.AddLocalReference<jobject>(reinterpret_cast<StackReference<mirror::Object>*>(&args[1])->AsMirrorPtr()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), klass.get(), arg0.get(), arg1.get());
    } else if (shorty == "IF") {
      using fntype = jint(JNIEnv*, jclass, jfloat);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetI(fn(soa.Env(), klass.get(), *reinterpret_cast<float*>(&args[0])));
    } else if (shorty == "JD") {
      using fntype = jlong(JNIEnv*, jclass, jdouble);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetJ(fn(soa.Env(), klass.get(), *reinterpret_cast<double*>(&args[0])));
    } else if (shorty == "DJ") {
      using fntype = jdouble(JNIEnv*, jclass, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetD(fn(soa.Env(), klass.get(), *reinterpret_cast<jlong*>(&args[0])));
    } else if (shorty == "DD") {
      // double fn(JNIEnv*, jclass, double) — Math.ceil, Math.floor, Math.sqrt, etc.
      using fntype = jdouble(JNIEnv*, jclass, jdouble);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jdouble arg0 = *reinterpret_cast<jdouble*>(&args[0]);
      result->SetD(fn(soa.Env(), klass.get(), arg0));
    } else if (shorty == "DDD") {
      // double fn(JNIEnv*, jclass, double, double) — Math.max, Math.min, Math.pow
      using fntype = jdouble(JNIEnv*, jclass, jdouble, jdouble);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jdouble arg0 = *reinterpret_cast<jdouble*>(&args[0]);
      jdouble arg1 = *reinterpret_cast<jdouble*>(&args[2]); // double takes 2 slots
      result->SetD(fn(soa.Env(), klass.get(), arg0, arg1));
    } else if (shorty == "FI") {
      using fntype = jfloat(JNIEnv*, jclass, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetF(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "LLIII") {
      using fntype = jobject(JNIEnv*, jclass, jobject, jint, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
          soa.AddLocalReference<jobject>(reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      ScopedLocalRef<jobject> r(soa.Env(), fn(soa.Env(), klass.get(), arg0.get(), args[1], args[2], args[3]));
      result->SetL(soa.Decode<mirror::Object>(r.get()));
    } else if (shorty == "VII") {
      using fntype = void(JNIEnv*, jclass, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), klass.get(), args[0], args[1]);
    } else if (shorty == "VL") {
      using fntype = void(JNIEnv*, jclass, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
          soa.AddLocalReference<jobject>(reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), klass.get(), arg0.get());
    } else if (shorty == "IL") {
      using fntype = jint(JNIEnv*, jclass, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
          soa.AddLocalReference<jobject>(reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetI(fn(soa.Env(), klass.get(), arg0.get()));
    } else if (shorty == "Z") {
      using fntype = jboolean(JNIEnv*, jclass);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetZ(fn(soa.Env(), klass.get()));
    } else if (shorty == "BI") {
      using fntype = jbyte(JNIEnv*, jclass, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetB(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "II") {
      using fntype = jint(JNIEnv*, jclass, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetI(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "LL") {
      using fntype = jobject(JNIEnv*, jclass, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jobject jresult;
      {
        // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
        jresult = fn(soa.Env(), klass.get(), arg0.get());
      }
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "IIZ") {
      using fntype = jint(JNIEnv*, jclass, jint, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetI(fn(soa.Env(), klass.get(), args[0], args[1]));
    } else if (shorty == "ILI") {
      using fntype = jint(JNIEnv*, jclass, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(
          method->GetEntryPointFromJni()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetI(fn(soa.Env(), klass.get(), arg0.get(), args[1]));
    } else if (shorty == "SIZ") {
      using fntype = jshort(JNIEnv*, jclass, jint, jboolean);
      fntype* const fn =
          reinterpret_cast<fntype*>(const_cast<void*>(method->GetEntryPointFromJni()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetS(fn(soa.Env(), klass.get(), args[0], args[1]));
    } else if (shorty == "VIZ") {
      using fntype = void(JNIEnv*, jclass, jint, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), klass.get(), args[0], args[1]);
    } else if (shorty == "ZLL") {
      using fntype = jboolean(JNIEnv*, jclass, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[1])));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetZ(fn(soa.Env(), klass.get(), arg0.get(), arg1.get()));
    } else if (shorty == "ZILL") {
      using fntype = jboolean(JNIEnv*, jclass, jint, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[1])));
      ScopedLocalRef<jobject> arg2(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[2])));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetZ(fn(soa.Env(), klass.get(), args[0], arg1.get(), arg2.get()));
    } else if (shorty == "VILII") {
      using fntype = void(JNIEnv*, jclass, jint, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[1])));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), klass.get(), args[0], arg1.get(), args[2], args[3]);
    } else if (shorty == "VLILII") {
      using fntype = void(JNIEnv*, jclass, jobject, jint, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg2(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[2])));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), klass.get(), arg0.get(), args[1], arg2.get(), args[3], args[4]);
    } else if (shorty == "JI") {
      using fntype = jlong(JNIEnv*, jclass, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      result->SetJ(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "VCLL") {
      // void System.log(char, String, Throwable)
      using fntype = void(JNIEnv*, jclass, jchar, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[1])));
      ScopedLocalRef<jobject> arg2(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[2])));
      fn(soa.Env(), klass.get(), args[0], arg1.get(), arg2.get());
    } else if (shorty == "LL") {
      // static Object method(Object)
      using fntype = jobject(JNIEnv*, jclass, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jobject jresult = fn(soa.Env(), klass.get(), arg0.get());
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "LLL") {
      // Object method(Object, Object) — e.g. SystemProperties.native_get(String, String)
      using fntype = jobject(JNIEnv*, jclass, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[1])));
      jobject jresult = fn(soa.Env(), klass.get(), arg0.get(), arg1.get());
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "LLZL") {
      // Class classForName(String, boolean, ClassLoader)
      using fntype = jobject(JNIEnv*, jclass, jobject, jboolean, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg2(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[2])));
      jobject jresult = fn(soa.Env(), klass.get(), arg0.get(), args[1], arg2.get());
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "ZLI") {
      // boolean fn(JNIEnv*, jclass, String, int) — e.g. Log.isLoggable
      using fntype = jboolean(JNIEnv*, jclass, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      result->SetZ(fn(soa.Env(), klass.get(), arg0.get(), args[1]));
    } else if (shorty == "ILLI") {
      // int fn(JNIEnv*, jclass, int, String, String, int) — e.g. Log.println_native
      using fntype = jint(JNIEnv*, jclass, jint, jint, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      result->SetI(fn(soa.Env(), klass.get(), args[0], args[1],
                       soa.AddLocalReference<jobject>(ObjArg(args[2])),
                       soa.AddLocalReference<jobject>(ObjArg(args[3]))));
    } else if (shorty == "IILL") {
      // int fn(JNIEnv*, jclass, int, int, String, String) — Log.println_native variant
      using fntype = jint(JNIEnv*, jclass, jint, jint, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg2(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[2])));
      ScopedLocalRef<jobject> arg3(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[3])));
      result->SetI(fn(soa.Env(), klass.get(), args[0], args[1], arg2.get(), arg3.get()));
    } else if (shorty == "ZLZ") {
      // boolean fn(JNIEnv*, jclass, String, boolean) — SystemProperties.native_get_boolean
      using fntype = jboolean(JNIEnv*, jclass, jobject, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      result->SetZ(fn(soa.Env(), klass.get(), arg0.get(), args[1]));
    } else if (shorty == "ILI") {
      // int fn(JNIEnv*, jclass, String, int) — SystemProperties.native_get_int
      using fntype = jint(JNIEnv*, jclass, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      result->SetI(fn(soa.Env(), klass.get(), arg0.get(), args[1]));
    } else if (shorty == "JLJ") {
      // long fn(JNIEnv*, jclass, String, long) — SystemProperties.native_get_long
      using fntype = jlong(JNIEnv*, jclass, jobject, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      result->SetJ(fn(soa.Env(), klass.get(), arg0.get(), arg1));
    } else if (shorty == "LLII") {
      // Object fn(JNIEnv*, jclass, Object, int, int) — StringFactory.newStringFromUtf8Bytes etc.
      using fntype = jobject(JNIEnv*, jclass, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> jresult(soa.Env(), fn(soa.Env(), klass.get(), arg0.get(), args[1], args[2]));
      result->SetL(soa.Decode<mirror::Object>(jresult.get()));
    } else if (shorty == "JJ") {
      // long fn(JNIEnv*, jclass, long) — e.g. ApkAssets.nativeGetStringBlock(long)
      using fntype = jlong(JNIEnv*, jclass, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      result->SetJ(fn(soa.Env(), klass.get(), arg0));
    } else if (shorty == "JILIL") {
      // long fn(JNIEnv*, jclass, int, String, int, Object) — ApkAssets.nativeLoad
      using fntype = jlong(JNIEnv*, jclass, jint, jobject, jint, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[1])));
      ScopedLocalRef<jobject> arg3(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[3])));
      result->SetJ(fn(soa.Env(), klass.get(), args[0], arg1.get(), args[2], arg3.get()));
    } else if (shorty == "I") {
      // int fn(JNIEnv*, jclass) — VMRuntime.getNotifyNativeInterval etc.
      using fntype = jint(JNIEnv*, jclass);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      result->SetI(fn(soa.Env(), klass.get()));
    } else if (shorty == "VJ") {
      // void fn(JNIEnv*, jclass, long) — Parcel.nativeFreeBuffer, nativeDestroy etc.
      using fntype = void(JNIEnv*, jclass, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      fn(soa.Env(), klass.get(), arg0);
    } else if (shorty == "VJII") {
      // void fn(JNIEnv*, jclass, long, int, int) — Canvas.nDrawColor etc.
      using fntype = void(JNIEnv*, jclass, jlong, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      fn(soa.Env(), klass.get(), arg0, args[2], args[3]);
    } else if (shorty == "VJL") {
      // void fn(JNIEnv*, jclass, long, Object) — Parcel.nativeMarkForBinder etc.
      using fntype = void(JNIEnv*, jclass, jlong, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      ScopedLocalRef<jobject> arg1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[2])));
      fn(soa.Env(), klass.get(), arg0, arg1.get());
    } else if (shorty == "IJL") {
      // int fn(JNIEnv*, jclass, long, Object) — Parcel.nativeReadInt etc.
      using fntype = jint(JNIEnv*, jclass, jlong, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      ScopedLocalRef<jobject> arg1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[2])));
      result->SetI(fn(soa.Env(), klass.get(), arg0, arg1.get()));
    } else if (shorty == "IJ") {
      // int fn(JNIEnv*, jclass, long) — Parcel.nativeReadInt(long)
      using fntype = jint(JNIEnv*, jclass, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      result->SetI(fn(soa.Env(), klass.get(), arg0));
    } else if (shorty == "LJ") {
      // Object fn(JNIEnv*, jclass, long) — Parcel.nativeReadString etc.
      using fntype = jobject(JNIEnv*, jclass, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      ScopedLocalRef<jobject> r(soa.Env(), fn(soa.Env(), klass.get(), arg0));
      result->SetL(soa.Decode<mirror::Object>(r.get()));
    } else if (shorty == "LLI") {
      // Object fn(JNIEnv*, jclass, Object, int) — Array.createObjectArray etc.
      using fntype = jobject(JNIEnv*, jclass, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> r(soa.Env(), fn(soa.Env(), klass.get(), arg0.get(), args[1]));
      result->SetL(soa.Decode<mirror::Object>(r.get()));
    } else if (shorty == "LIIL") {
      // Object fn(JNIEnv*, jclass, int, int, Object) — StringFactory.newStringFromChars
      using fntype = jobject(JNIEnv*, jclass, jint, jint, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg2(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[2])));
      ScopedLocalRef<jobject> r(soa.Env(), fn(soa.Env(), klass.get(), args[0], args[1], arg2.get()));
      result->SetL(soa.Decode<mirror::Object>(r.get()));
    } else if (shorty == "VLLJ") {
      // void fn(JNIEnv*, jclass, Object, Object, long) — McdLoader.nativeSetApkAssets etc.
      using fntype = void(JNIEnv*, jclass, jobject, jobject, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[1])));
      jlong arg2 = *reinterpret_cast<jlong*>(&args[2]);
      fn(soa.Env(), klass.get(), arg0.get(), arg1.get(), arg2);
    } else if (shorty == "VJI") {
      // void fn(JNIEnv*, jclass, long, int) — Parcel.nativeWriteInt etc.
      using fntype = void(JNIEnv*, jclass, jlong, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      fn(soa.Env(), klass.get(), arg0, args[2]);
    } else if (shorty == "VJJ") {
      // void fn(JNIEnv*, jclass, long, long) — NativeAllocationRegistry.applyFreeFunction
      using fntype = void(JNIEnv*, jclass, jlong, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      jlong arg1 = *reinterpret_cast<jlong*>(&args[2]);
      fn(soa.Env(), klass.get(), arg0, arg1);
    } else if (shorty == "JJLL") {
      // long fn(JNIEnv*, jclass, long, Object, Object) — Surface.nativeLockCanvas
      using fntype = jlong(JNIEnv*, jclass, jlong, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(), soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      ScopedLocalRef<jobject> a1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[2])));
      ScopedLocalRef<jobject> a2(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[3])));
      result->SetJ(fn(soa.Env(), klass.get(), arg0, a1.get(), a2.get()));
    } else if (shorty == "JLLIIIIJL") {
      // long fn(JNIEnv*, jclass, Object, Object, int, int, int, int, long, Object)
      // SurfaceControl.nativeCreate(Session, name, w, h, format, flags, parentPtr, metadata)
      using fntype = jlong(JNIEnv*, jclass, jobject, jobject, jint, jint, jint, jint, jlong, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(), soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> a0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> a1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[1])));
      jlong a5 = *reinterpret_cast<jlong*>(&args[6]); // after 4 ints + 2 obj slots = offset 6
      ScopedLocalRef<jobject> a6(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[8])));
      result->SetJ(fn(soa.Env(), klass.get(), a0.get(), a1.get(), args[2], args[3], args[4], args[5], a5, a6.get()));
    } else if (shorty == "VJLZZ") {
      // void fn(JNIEnv*, jclass, long, Object[], boolean, boolean) — AssetManager.nativeSetApkAssets
      using fntype = void(JNIEnv*, jclass, jlong, jobjectArray, jboolean, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[2]))); // after long (2 slots)
      fn(soa.Env(), klass.get(), arg0, (jobjectArray)arg1.get(), (jboolean)args[3], (jboolean)args[4]);
    } else if (shorty == "ZJ") {
      // boolean fn(JNIEnv*, jclass, long) — Trace.nativeIsTagEnabled
      using fntype = jboolean(JNIEnv*, jclass, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      result->SetZ(fn(soa.Env(), klass.get(), arg0));
    } else {
      LOG(WARNING) << "InterpreterJni: unhandled static shorty '" << shorty << "' for " << method->PrettyMethod();
    }
  } else {
    if (shorty == "VI") {
      // void method(int) — e.g. Runtime.halt(int)
      using fntype = void(JNIEnv*, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      fn(soa.Env(), rcvr.get(), args[0]);
    } else if (shorty == "VL") {
      // void method(Object) — e.g. Thread.setName(String)
      using fntype = void(JNIEnv*, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      fn(soa.Env(), rcvr.get(), arg0.get());
    } else if (shorty == "L") {
      using fntype = jobject(JNIEnv*, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      static int l_count = 0;
      if (fn == nullptr || ++l_count <= 20) {
        fprintf(stderr, "[InterpJni] L: fn=%p rcvr=%p for %s\n",
                (void*)fn, receiver.Ptr(), method->PrettyMethod().c_str());
        fflush(stderr);
      }
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      jobject jresult;
      {
        // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
        jresult = fn(soa.Env(), rcvr.get());
      }
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "LLL" && !method->IsStatic()) {
      // Object method(Object, Object) — non-static
      using fntype = jobject(JNIEnv*, jobject, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[1])));
      jobject jresult = fn(soa.Env(), rcvr.get(), arg0.get(), arg1.get());
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "V") {
      using fntype = void(JNIEnv*, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), rcvr.get());
    } else if (shorty == "LL") {
      using fntype = jobject(JNIEnv*, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jobject jresult;
      {
        // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
        jresult = fn(soa.Env(), rcvr.get(), arg0.get());
      }
      result->SetL(soa.Decode<mirror::Object>(jresult));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
    } else if (shorty == "CI") {
      using fntype = jchar(JNIEnv*, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
          soa.AddLocalReference<jobject>(receiver));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetC(fn(soa.Env(), rcvr.get(), args[0]));
    } else if (shorty == "LZ") {
      // Object getDeclaredMethodsUnchecked(boolean)
      using fntype = jobject(JNIEnv*, jobject, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      jobject jresult = fn(soa.Env(), rcvr.get(), args[0]);
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "I" && !method->IsStatic()) {
      using fntype = jint(JNIEnv*, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
          soa.AddLocalReference<jobject>(receiver));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetI(fn(soa.Env(), rcvr.get()));
    } else if (shorty == "VLI") {
      using fntype = void(JNIEnv*, jobject, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
          soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
          soa.AddLocalReference<jobject>(reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr()));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      fn(soa.Env(), rcvr.get(), arg0.get(), args[1]);
    } else if (shorty == "III") {
      using fntype = jint(JNIEnv*, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetI(fn(soa.Env(), rcvr.get(), args[0], args[1]));
    } else if (shorty == "ZL") {
      using fntype = jboolean(JNIEnv*, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetZ(fn(soa.Env(), rcvr.get(), arg0.get()));
    } else if (shorty == "LII") {
      // String fastSubstring(int, int)
      using fntype = jobject(JNIEnv*, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      jobject jresult = fn(soa.Env(), rcvr.get(), args[0], args[1]);
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "LI") {
      using fntype = jobject(JNIEnv*, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      jobject jresult = fn(soa.Env(), rcvr.get(), args[0]);
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "ZLJII") {
      using fntype = jboolean(JNIEnv*, jobject, jobject, jlong, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetZ(fn(soa.Env(), rcvr.get(), arg0.get(), arg1, args[3], args[4]));
    } else if (shorty == "JLL") {
      using fntype = jlong(JNIEnv*, jobject, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[1])));
      // No state transition: stay in kRunnable for FastNative compat + nonconcurrent GC
      result->SetJ(fn(soa.Env(), rcvr.get(), arg0.get(), arg1.get()));
    } else if (shorty == "LLJ") {
      // Object getReferenceVolatile(Object, long)
      using fntype = jobject(JNIEnv*, jobject, jobject, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      jobject jresult = fn(soa.Env(), rcvr.get(), arg0.get(), arg1);
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "ZLJLL") {
      // boolean compareAndSetReference(Object, long, Object, Object)
      using fntype = jboolean(JNIEnv*, jobject, jobject, jlong, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      ScopedLocalRef<jobject> arg3(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[3])));
      ScopedLocalRef<jobject> arg4(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[4])));
      result->SetZ(fn(soa.Env(), rcvr.get(), arg0.get(), arg1, arg3.get(), arg4.get()));
    } else if (shorty == "ZLJII") {
      // boolean compareAndSetInt(Object, long, int, int)
      using fntype = jboolean(JNIEnv*, jobject, jobject, jlong, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      result->SetZ(fn(soa.Env(), rcvr.get(), arg0.get(), arg1, args[3], args[4]));
    } else if (shorty == "VLJLL") {
      // void putReferenceVolatile(Object, long, Object)
      using fntype = void(JNIEnv*, jobject, jobject, jlong, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      ScopedLocalRef<jobject> arg3(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[3])));
      fn(soa.Env(), rcvr.get(), arg0.get(), arg1, arg3.get());
    } else if (shorty == "VLJI") {
      // void putIntVolatile(Object, long, int)
      using fntype = void(JNIEnv*, jobject, jobject, jlong, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      fn(soa.Env(), rcvr.get(), arg0.get(), arg1, args[3]);
    } else if (shorty == "ILJ") {
      // int getIntVolatile(Object, long)
      using fntype = jint(JNIEnv*, jobject, jobject, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      result->SetI(fn(soa.Env(), rcvr.get(), arg0.get(), arg1));
    } else if (shorty == "ZLJJJ") {
      // boolean compareAndSetLong(Object, long, long, long)
      using fntype = jboolean(JNIEnv*, jobject, jobject, jlong, jlong, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      jlong arg3 = *reinterpret_cast<jlong*>(&args[3]);
      jlong arg5 = *reinterpret_cast<jlong*>(&args[5]);
      result->SetZ(fn(soa.Env(), rcvr.get(), arg0.get(), arg1, arg3, arg5));
    } else if (shorty == "JLJ") {
      // long getLongVolatile(Object, long)
      using fntype = jlong(JNIEnv*, jobject, jobject, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      result->SetJ(fn(soa.Env(), rcvr.get(), arg0.get(), arg1));
    } else if (shorty == "VLJJ") {
      // void putLongVolatile(Object, long, long)
      using fntype = void(JNIEnv*, jobject, jobject, jlong, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      jlong arg3 = *reinterpret_cast<jlong*>(&args[3]);
      fn(soa.Env(), rcvr.get(), arg0.get(), arg1, arg3);
    } else if (shorty == "VLJL") {
      // void putReferenceVolatile(Object, long, Object)
      using fntype = void(JNIEnv*, jobject, jobject, jlong, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      ScopedLocalRef<jobject> arg3(soa.Env(),
                                   soa.AddLocalReference<jobject>(ObjArg(args[3])));
      fn(soa.Env(), rcvr.get(), arg0.get(), arg1, arg3.get());
    } else if (shorty == "JI") {
      // long sysconf(int) - non-static
      using fntype = jlong(JNIEnv*, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      result->SetJ(fn(soa.Env(), rcvr.get(), args[0]));
    } else if (shorty == "ILLII") {
      // int writeBytes(FileDescriptor, Object, int, int) — I/O write
      using fntype = jint(JNIEnv*, jobject, jobject, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[1])));
      result->SetI(fn(soa.Env(), rcvr.get(), arg0.get(), arg1.get(), args[2], args[3]));
    } else if (shorty == "VIILI") {
      // void String.getCharsNoCheck(int, int, char[], int)
      using fntype = void(JNIEnv*, jobject, jint, jint, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg2(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[2])));
      fn(soa.Env(), rcvr.get(), args[0], args[1], arg2.get(), args[3]);
    } else if (shorty == "VLL") {
      // void fn(JNIEnv*, jobject, Object, Object) — e.g. Field.set
      using fntype = void(JNIEnv*, jobject, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[1])));
      fn(soa.Env(), rcvr.get(), arg0.get(), arg1.get());
    } else if (shorty == "II") {
      // int fn(JNIEnv*, jobject, int) — e.g. String.lastIndexOf(int)
      using fntype = jint(JNIEnv*, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      result->SetI(fn(soa.Env(), rcvr.get(), args[0]));
    } else if (shorty == "IZ") {
      // int fn(JNIEnv*, jobject, boolean) — e.g. Thread.nativeGetStatus
      using fntype = jint(JNIEnv*, jobject, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      result->SetI(fn(soa.Env(), rcvr.get(), args[0]));
    } else if (shorty == "LLI") {
      // Object fn(JNIEnv*, jobject, Object, int) — e.g. Field.get with index
      using fntype = jobject(JNIEnv*, jobject, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jobject jresult = fn(soa.Env(), rcvr.get(), arg0.get(), args[1]);
      result->SetL(soa.Decode<mirror::Object>(jresult));
    } else if (shorty == "Z") {
      // boolean fn(JNIEnv*, jobject) — e.g. Activity.isTaskRoot
      using fntype = jboolean(JNIEnv*, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      result->SetZ(fn(soa.Env(), rcvr.get()));
    } else if (shorty == "IL") {
      // int fn(JNIEnv*, jobject, Object) — e.g. Field.getInt(Object)
      using fntype = jint(JNIEnv*, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      result->SetI(fn(soa.Env(), rcvr.get(), arg0.get()));
    } else if (shorty == "LJ") {
      // Object fn(JNIEnv*, jobject, long) — BinderProxy$ProxyMap.get(long) etc.
      using fntype = jobject(JNIEnv*, jobject, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      ScopedLocalRef<jobject> r(soa.Env(), fn(soa.Env(), rcvr.get(), arg0));
      result->SetL(soa.Decode<mirror::Object>(r.get()));
    } else if (shorty == "ZILLI") {
      // boolean fn(JNIEnv*, jobject, int, Object, Object, int) — BinderProxy.transactNative!
      using fntype = jboolean(JNIEnv*, jobject, jint, jobject, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg1(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[1])));
      ScopedLocalRef<jobject> arg2(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[2])));
      result->SetZ(fn(soa.Env(), rcvr.get(), args[0], arg1.get(), arg2.get(), args[3]));
    } else if (shorty == "VLJ") {
      // void fn(JNIEnv*, jobject, Object, long) — Field.setLong etc.
      using fntype = void(JNIEnv*, jobject, jobject, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      jlong arg1 = *reinterpret_cast<jlong*>(&args[1]);
      fn(soa.Env(), rcvr.get(), arg0.get(), arg1);
    } else if (shorty == "VJ") {
      // void fn(JNIEnv*, jobject, long) — VMRuntime.registerNativeAllocation
      using fntype = void(JNIEnv*, jobject, jlong);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      jlong arg0 = *reinterpret_cast<jlong*>(&args[0]);
      fn(soa.Env(), rcvr.get(), arg0);
    } else if (shorty == "VLF") {
      // void fn(JNIEnv*, jobject, Object, float) — Field.setFloat
      using fntype = void(JNIEnv*, jobject, jobject, jfloat);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      fn(soa.Env(), rcvr.get(), arg0.get(), *reinterpret_cast<jfloat*>(&args[1]));
    } else if (shorty == "VLI") {
      // void fn(JNIEnv*, jobject, Object, int) — Field.setInt etc.
      using fntype = void(JNIEnv*, jobject, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      fn(soa.Env(), rcvr.get(), arg0.get(), args[1]);
    } else if (shorty == "VLIZ") {
      // void fn(JNIEnv*, jobject, Object, int, boolean) — Activity.onApplyThemeResource
      using fntype = void(JNIEnv*, jobject, jobject, jint, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(), soa.AddLocalReference<jobject>(ObjArg(args[0])));
      fn(soa.Env(), rcvr.get(), arg0.get(), args[1], (jboolean)args[2]);
    } else if (shorty == "I") {
      // int fn(JNIEnv*, jobject) — hashCode, etc.
      using fntype = jint(JNIEnv*, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      result->SetI(fn(soa.Env(), rcvr.get()));
    } else if (shorty == "J") {
      // long fn(JNIEnv*, jobject) — e.g. currentTimeMillis on instance
      using fntype = jlong(JNIEnv*, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(method->GetEntryPointFromJni());
      ScopedLocalRef<jobject> rcvr(soa.Env(), soa.AddLocalReference<jobject>(receiver));
      result->SetJ(fn(soa.Env(), rcvr.get()));
    } else {
      LOG(WARNING) << "InterpreterJni: unhandled non-static shorty '" << shorty << "' for " << method->PrettyMethod();
    }
  }
}

NO_STACK_PROTECTOR
static JValue ExecuteSwitch(Thread* self,
                            const CodeItemDataAccessor& accessor,
                            ShadowFrame& shadow_frame,
                            JValue result_register,
                            bool interpret_one_instruction) REQUIRES_SHARED(Locks::mutator_lock_) {
  Runtime* runtime = Runtime::Current();
  auto switch_impl_cpp = runtime->IsActiveTransaction()
      ? runtime->GetClassLinker()->GetTransactionalInterpreter()
      : reinterpret_cast<const void*>(&ExecuteSwitchImplCpp</*transaction_active=*/ false>);
  return ExecuteSwitchImpl(
      self, accessor, shadow_frame, result_register, interpret_one_instruction, switch_impl_cpp);
}

NO_STACK_PROTECTOR
static inline JValue Execute(
    Thread* self,
    const CodeItemDataAccessor& accessor,
    ShadowFrame& shadow_frame,
    JValue result_register,
    bool stay_in_interpreter = false,
    bool from_deoptimize = false) REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(!shadow_frame.GetMethod()->IsAbstract());
  DCHECK(!shadow_frame.GetMethod()->IsNative());

  // We cache the result of NeedsDexPcEvents in the shadow frame so we don't need to call
  // NeedsDexPcEvents on every instruction for better performance. NeedsDexPcEvents only gets
  // updated asynchronoulsy in a SuspendAll scope and any existing shadow frames are updated with
  // new value. So it is safe to cache it here.
  shadow_frame.SetNotifyDexPcMoveEvents(
      Runtime::Current()->GetInstrumentation()->NeedsDexPcEvents(shadow_frame.GetMethod(), self));

  if (LIKELY(!from_deoptimize)) {  // Entering the method, but not via deoptimization.
    if (kIsDebugBuild) {
      CHECK_EQ(shadow_frame.GetDexPC(), 0u);
      self->AssertNoPendingException();
    }
    ArtMethod *method = shadow_frame.GetMethod();

    // If we can continue in JIT and have JITed code available execute JITed code.
    if (!stay_in_interpreter &&
        !self->IsForceInterpreter() &&
        !shadow_frame.GetForcePopFrame() &&
        !shadow_frame.GetNotifyDexPcMoveEvents()) {
      jit::Jit* jit = Runtime::Current()->GetJit();
      if (jit != nullptr) {
        jit->MethodEntered(self, shadow_frame.GetMethod());
        if (jit->CanInvokeCompiledCode(method)) {
          JValue result;

          // Pop the shadow frame before calling into compiled code.
          self->PopShadowFrame();
          // Calculate the offset of the first input reg. The input registers are in the high regs.
          // It's ok to access the code item here since JIT code will have been touched by the
          // interpreter and compiler already.
          uint16_t arg_offset = accessor.RegistersSize() - accessor.InsSize();
          ArtInterpreterToCompiledCodeBridge(self, nullptr, &shadow_frame, arg_offset, &result);
          // Push the shadow frame back as the caller will expect it.
          self->PushShadowFrame(&shadow_frame);

          return result;
        }
      }
    }

    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    if (UNLIKELY(instrumentation->HasMethodEntryListeners() || shadow_frame.GetForcePopFrame())) {
      instrumentation->MethodEnterEvent(self, method);
      if (UNLIKELY(shadow_frame.GetForcePopFrame())) {
        // The caller will retry this invoke or ignore the result. Just return immediately without
        // any value.
        DCHECK(Runtime::Current()->AreNonStandardExitsEnabled());
        JValue ret = JValue();
        PerformNonStandardReturn(self,
                                 shadow_frame,
                                 ret,
                                 instrumentation,
                                 /* unlock_monitors= */ false);
        return ret;
      }
      if (UNLIKELY(self->IsExceptionPending())) {
        instrumentation->MethodUnwindEvent(self,
                                           method,
                                           0);
        JValue ret = JValue();
        if (UNLIKELY(shadow_frame.GetForcePopFrame())) {
          DCHECK(Runtime::Current()->AreNonStandardExitsEnabled());
          PerformNonStandardReturn(self,
                                   shadow_frame,
                                   ret,
                                   instrumentation,
                                   /* unlock_monitors= */ false);
        }
        return ret;
      }
    }
  }

  ArtMethod* method = shadow_frame.GetMethod();

  DCheckStaticState(self, method);

  // Lock counting is a special version of accessibility checks, and for simplicity and
  // reduction of template parameters, we gate it behind access-checks mode.
  DCHECK_IMPLIES(method->SkipAccessChecks(), !method->MustCountLocks());

  VLOG(interpreter) << "Interpreting " << method->PrettyMethod();

  return ExecuteSwitch(
      self, accessor, shadow_frame, result_register, /*interpret_one_instruction=*/ false);
}

void EnterInterpreterFromInvoke(Thread* self,
                                ArtMethod* method,
                                ObjPtr<mirror::Object> receiver,
                                uint32_t* args,
                                JValue* result,
                                bool stay_in_interpreter) {
  DCHECK_EQ(self, Thread::Current());

  // Interpreter depth guard — prevents infinite recursion from circular class init
  static thread_local int invoke_depth = 0;
  invoke_depth++;
  struct InvokeDepthGuard { ~InvokeDepthGuard() { invoke_depth--; } } idg;
  if (invoke_depth > 50) {
    // Don't decrement here — RAII guard handles it
    ThrowStackOverflowError(self);
    return;
  }

  bool implicit_check = Runtime::Current()->GetImplicitStackOverflowChecks();
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEndForInterpreter(implicit_check))) {
    ThrowStackOverflowError(self);
    return;
  }

  // This can happen if we are in forced interpreter mode and an obsolete method is called using
  // reflection.
  if (UNLIKELY(method->IsObsolete())) {
    ThrowInternalError("Attempting to invoke obsolete version of '%s'.",
                       method->PrettyMethod().c_str());
    return;
  }

  const char* old_cause = self->StartAssertNoThreadSuspension("EnterInterpreterFromInvoke");
  CodeItemDataAccessor accessor(method->DexInstructionData());
  uint16_t num_regs;
  uint16_t num_ins;
  if (accessor.HasCodeItem()) {
    num_regs =  accessor.RegistersSize();
    num_ins = accessor.InsSize();
  } else if (!method->IsInvokable()) {
    self->EndAssertNoThreadSuspension(old_cause);
    method->ThrowInvocationTimeError(receiver);
    return;
  } else {
    DCHECK(method->IsNative()) << method->PrettyMethod();
    num_regs = num_ins = ArtMethod::NumArgRegisters(method->GetShortyView());
    if (!method->IsStatic()) {
      num_regs++;
      num_ins++;
    }
  }
  // Set up shadow frame with matching number of reference slots to vregs.
  ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
      CREATE_SHADOW_FRAME(num_regs, method, /* dex pc */ 0);
  ShadowFrame* shadow_frame = shadow_frame_unique_ptr.get();

  size_t cur_reg = num_regs - num_ins;
  if (!method->IsStatic()) {
    CHECK(receiver != nullptr);
    shadow_frame->SetVRegReference(cur_reg, receiver);
    ++cur_reg;
  }
  uint32_t shorty_len = 0;
  const char* shorty = method->GetShorty(&shorty_len);
  // DEBUG: trace shorty for non-boot-image methods
  if (method->IsNative()) {
    fprintf(stderr, "[InterpJni] %s shorty='%s' len=%u dexIdx=%u\n",
            method->PrettyMethod().c_str(), shorty ? shorty : "NULL", shorty_len,
            method->GetDexMethodIndex());
    fflush(stderr);
  }
  for (size_t shorty_pos = 0, arg_pos = 0; cur_reg < num_regs; ++shorty_pos, ++arg_pos, cur_reg++) {
    DCHECK_LT(shorty_pos + 1, shorty_len);
    switch (shorty[shorty_pos + 1]) {
      case 'L': {
        ObjPtr<mirror::Object> o =
            reinterpret_cast<StackReference<mirror::Object>*>(&args[arg_pos])->AsMirrorPtr();
        shadow_frame->SetVRegReference(cur_reg, o);
        break;
      }
      case 'J': case 'D': {
        uint64_t wide_value = (static_cast<uint64_t>(args[arg_pos + 1]) << 32) | args[arg_pos];
        shadow_frame->SetVRegLong(cur_reg, wide_value);
        cur_reg++;
        arg_pos++;
        break;
      }
      default:
        shadow_frame->SetVReg(cur_reg, args[arg_pos]);
        break;
    }
  }
  self->EndAssertNoThreadSuspension(old_cause);
  if (!EnsureInitialized(self, shadow_frame)) {
    return;
  }
  self->PushShadowFrame(shadow_frame);
  if (LIKELY(!method->IsNative())) {
    JValue r = Execute(self, accessor, *shadow_frame, JValue(), stay_in_interpreter);
    if (result != nullptr) {
      *result = r;
    }
  } else {
    // We don't expect to be asked to interpret native code (which is entered via a JNI compiler
    // generated stub) except during testing and image writing.
    // Update args to be the args in the shadow frame since the input ones could hold stale
    // references pointers due to moving GC.
    args = shadow_frame->GetVRegArgs(method->IsStatic() ? 0 : 1);
    // Always use InterpreterJni (not UnstartedRuntime::Jni) — our native stubs
    // are registered and available even before Runtime::IsStarted(). UnstartedRuntime::Jni
    // only handles a subset of methods and returns null for unhandled ones.
    if (!Runtime::Current()->IsStarted() && Runtime::Current()->IsAotCompiler()) {
      UnstartedRuntime::Jni(self, method, receiver.Ptr(), args, result);
    } else {
      InterpreterJni(self, method, shorty, receiver, args, result);
    }
  }
  self->PopShadowFrame();
}

static int16_t GetReceiverRegisterForStringInit(const Instruction* instr) {
  DCHECK(instr->Opcode() == Instruction::INVOKE_DIRECT_RANGE ||
         instr->Opcode() == Instruction::INVOKE_DIRECT);
  return (instr->Opcode() == Instruction::INVOKE_DIRECT_RANGE) ?
      instr->VRegC_3rc() : instr->VRegC_35c();
}

void EnterInterpreterFromDeoptimize(Thread* self,
                                    ShadowFrame* shadow_frame,
                                    JValue* ret_val,
                                    bool from_code,
                                    DeoptimizationMethodType deopt_method_type)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  JValue value;
  // Set value to last known result in case the shadow frame chain is empty.
  value.SetJ(ret_val->GetJ());
  // How many frames we have executed.
  size_t frame_cnt = 0;
  while (shadow_frame != nullptr) {
    // We do not want to recover lock state for lock counting when deoptimizing. Currently,
    // the compiler should not have compiled a method that failed structured-locking checks.
    DCHECK(!shadow_frame->GetMethod()->MustCountLocks());

    self->SetTopOfShadowStack(shadow_frame);
    CodeItemDataAccessor accessor(shadow_frame->GetMethod()->DexInstructionData());
    const uint32_t dex_pc = shadow_frame->GetDexPC();
    uint32_t new_dex_pc = dex_pc;
    if (UNLIKELY(self->IsExceptionPending())) {
      DCHECK(self->GetException() != Thread::GetDeoptimizationException());
      // If we deoptimize from the QuickExceptionHandler, we already reported the exception throw
      // event to the instrumentation. Skip throw listeners for the first frame. The deopt check
      // should happen after the throw listener is called as throw listener can trigger a
      // deoptimization.
      new_dex_pc = MoveToExceptionHandler(self,
                                          *shadow_frame,
                                          /* skip_listeners= */ false,
                                          /* skip_throw_listener= */ frame_cnt == 0) ?
                       shadow_frame->GetDexPC() :
                       dex::kDexNoIndex;
    } else if (!from_code) {
      // Deoptimization is not called from code directly.
      const Instruction* instr = &accessor.InstructionAt(dex_pc);
      if (deopt_method_type == DeoptimizationMethodType::kKeepDexPc ||
          shadow_frame->GetForceRetryInstruction()) {
        DCHECK(frame_cnt == 0 || shadow_frame->GetForceRetryInstruction())
            << "frame_cnt: " << frame_cnt
            << " force-retry: " << shadow_frame->GetForceRetryInstruction();
        // Need to re-execute the dex instruction.
        // (1) An invocation might be split into class initialization and invoke.
        //     In this case, the invoke should not be skipped.
        // (2) A suspend check should also execute the dex instruction at the
        //     corresponding dex pc.
        // If the ForceRetryInstruction bit is set this must be the second frame (the first being
        // the one that is being popped).
        DCHECK_EQ(new_dex_pc, dex_pc);
        shadow_frame->SetForceRetryInstruction(false);
      } else if (instr->Opcode() == Instruction::MONITOR_ENTER ||
                 instr->Opcode() == Instruction::MONITOR_EXIT) {
        DCHECK(deopt_method_type == DeoptimizationMethodType::kDefault);
        DCHECK_EQ(frame_cnt, 0u);
        // Non-idempotent dex instruction should not be re-executed.
        // On the other hand, if a MONITOR_ENTER is at the dex_pc of a suspend
        // check, that MONITOR_ENTER should be executed. That case is handled
        // above.
        new_dex_pc = dex_pc + instr->SizeInCodeUnits();
      } else if (instr->IsInvoke()) {
        DCHECK(deopt_method_type == DeoptimizationMethodType::kDefault);
        if (IsStringInit(*instr, shadow_frame->GetMethod())) {
          uint16_t this_obj_vreg = GetReceiverRegisterForStringInit(instr);
          // Move the StringFactory.newStringFromChars() result into the register representing
          // "this object" when invoking the string constructor in the original dex instruction.
          // Also move the result into all aliases.
          DCHECK(value.GetL()->IsString());
          SetStringInitValueToAllAliases(shadow_frame, this_obj_vreg, value);
          // Calling string constructor in the original dex code doesn't generate a result value.
          value.SetJ(0);
        }
        new_dex_pc = dex_pc + instr->SizeInCodeUnits();
      } else if (instr->Opcode() == Instruction::NEW_INSTANCE) {
        // A NEW_INSTANCE is simply re-executed, including
        // "new-instance String" which is compiled into a call into
        // StringFactory.newEmptyString().
        DCHECK_EQ(new_dex_pc, dex_pc);
      } else {
        DCHECK(deopt_method_type == DeoptimizationMethodType::kDefault);
        DCHECK_EQ(frame_cnt, 0u);
        // By default, we re-execute the dex instruction since if they are not
        // an invoke, so that we don't have to decode the dex instruction to move
        // result into the right vreg. All slow paths have been audited to be
        // idempotent except monitor-enter/exit and invocation stubs.
        // TODO: move result and advance dex pc. That also requires that we
        // can tell the return type of a runtime method, possibly by decoding
        // the dex instruction at the caller.
        DCHECK_EQ(new_dex_pc, dex_pc);
      }
    } else {
      // Nothing to do, the dex_pc is the one at which the code requested
      // the deoptimization.
      DCHECK_EQ(frame_cnt, 0u);
      DCHECK_EQ(new_dex_pc, dex_pc);
    }
    if (new_dex_pc != dex::kDexNoIndex) {
      shadow_frame->SetDexPC(new_dex_pc);
      value = Execute(self,
                      accessor,
                      *shadow_frame,
                      value,
                      /* stay_in_interpreter= */ true,
                      /* from_deoptimize= */ true);
    }
    ShadowFrame* old_frame = shadow_frame;
    shadow_frame = shadow_frame->GetLink();
    ShadowFrame::DeleteDeoptimizedFrame(old_frame);
    // Following deoptimizations of shadow frames must be at invocation point
    // and should advance dex pc past the invoke instruction.
    from_code = false;
    deopt_method_type = DeoptimizationMethodType::kDefault;
    frame_cnt++;
  }
  ret_val->SetJ(value.GetJ());
}

NO_STACK_PROTECTOR
JValue EnterInterpreterFromEntryPoint(Thread* self, const CodeItemDataAccessor& accessor,
                                      ShadowFrame* shadow_frame) {
  DCHECK_EQ(self, Thread::Current());
  bool implicit_check = Runtime::Current()->GetImplicitStackOverflowChecks();
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEndForInterpreter(implicit_check))) {
    ThrowStackOverflowError(self);
    return JValue();
  }

  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    jit->NotifyCompiledCodeToInterpreterTransition(self, shadow_frame->GetMethod());
  }
  return Execute(self, accessor, *shadow_frame, JValue());
}

NO_STACK_PROTECTOR
void ArtInterpreterToInterpreterBridge(Thread* self,
                                       const CodeItemDataAccessor& accessor,
                                       ShadowFrame* shadow_frame,
                                       JValue* result) {
  bool implicit_check = Runtime::Current()->GetImplicitStackOverflowChecks();
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEndForInterpreter(implicit_check))) {
    ThrowStackOverflowError(self);
    return;
  }

  self->PushShadowFrame(shadow_frame);

  if (LIKELY(!shadow_frame->GetMethod()->IsNative())) {
    result->SetJ(Execute(self, accessor, *shadow_frame, JValue()).GetJ());
  } else {
    // We don't expect to be asked to interpret native code (which is entered via a JNI compiler
    // generated stub) except during testing and image writing.
    CHECK(!Runtime::Current()->IsStarted());
    bool is_static = shadow_frame->GetMethod()->IsStatic();
    ObjPtr<mirror::Object> receiver = is_static ? nullptr : shadow_frame->GetVRegReference(0);
    uint32_t* args = shadow_frame->GetVRegArgs(is_static ? 0 : 1);
    UnstartedRuntime::Jni(self, shadow_frame->GetMethod(), receiver.Ptr(), args, result);
  }

  self->PopShadowFrame();
}

void CheckInterpreterAsmConstants() {
  CheckNterpAsmConstants();
}

bool PrevFrameWillRetry(Thread* self, const ShadowFrame& frame) {
  ShadowFrame* prev_frame = frame.GetLink();
  if (prev_frame == nullptr) {
    NthCallerVisitor vis(self, 1, false);
    vis.WalkStack();
    prev_frame = vis.GetCurrentShadowFrame();
    if (prev_frame == nullptr) {
      prev_frame = self->FindDebuggerShadowFrame(vis.GetFrameId());
    }
  }
  return prev_frame != nullptr && prev_frame->GetForceRetryInstruction();
}

}  // namespace interpreter
}  // namespace art
