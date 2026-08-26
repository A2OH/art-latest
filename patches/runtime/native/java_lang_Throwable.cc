/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "java_lang_Throwable.h"

#include <cstdio>

#include <unistd.h>

#include "nativehelper/jni_macros.h"

#include "jni/jni_internal.h"
#include "tolerant_native_util.h"
#include "scoped_fast_native_object_access-inl.h"
#include "thread.h"

namespace art HIDDEN {

static jobject Throwable_nativeFillInStackTrace(JNIEnv* env, jclass) {
  // WESTLAKE §661 (2026-08-16): this noop is why NOTHING on this port has a usable stack trace —
  // `getStackTrace()` returns nothing, `[INITCHILD-FAIL]` prints messages with 0 frames, and app
  // exceptions (e.g. Toutiao's Kotlin-intrinsic NPE "…ViewModelProvider.get, parameter modelClass")
  // cannot be located at all. Returning nullptr leaves Throwable.stackState empty forever.
  //
  // ⚠️THE FILE THAT COUNTS: `Makefile.ohos-arm64:240` excludes ALL `$(ART)/runtime/native/*.cc` and
  // compiles the copies under `patches/runtime/native/` instead. That is the OPPOSITE of
  // `common_throws.cc`, where the patches/ copy is dead and $(ART) is built (§657). ★The rule is
  // PER-DIRECTORY — always `grep -n <basename> Makefile.ohos-arm64` before editing either tree.
  //
  // Re-enabled behind a FILE trigger (env vars do not reach appspawn-x children):
  //     touch /data/local/tmp/asx/THROWTRACE
  // ⚠️Hard-gated because this runs on EVERY Throwable construction; §650 is the standing lesson
  // that hot-path work can dominate wall-clock and fake progress.
  {
    static thread_local int wl_state = -1;
    if (wl_state < 0) {
      wl_state = (access("/data/local/tmp/asx/THROWTRACE", F_OK) == 0) ? 1 : 0;
    }
    if (wl_state == 1) {
      ScopedFastNativeObjectAccess soa(env);
      return soa.AddLocalReference<jobject>(soa.Self()->CreateInternalStackTrace(soa));
    }
  }
  (void)env;
  static int fill_noop_count = 0;
  if (fill_noop_count < 20) {
    fill_noop_count++;
    fprintf(stderr, "[PFCUT] Throwable.nativeFillInStackTrace noop\n");
    fflush(stderr);
  }
  return nullptr;
}

static jobjectArray Throwable_nativeGetStackTrace(JNIEnv* env, jclass, jobject javaStackState) {
  if (javaStackState == nullptr) {
      return nullptr;
  }
  ScopedFastNativeObjectAccess soa(env);
  return Thread::InternalStackTraceToStackTraceElementArray(soa, javaStackState);
}

static JNINativeMethod gMethods[] = {
  FAST_NATIVE_METHOD(Throwable, nativeFillInStackTrace, "()Ljava/lang/Object;"),
  FAST_NATIVE_METHOD(Throwable, nativeGetStackTrace, "(Ljava/lang/Object;)[Ljava/lang/StackTraceElement;"),
};

void register_java_lang_Throwable(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Throwable");
}

}  // namespace art
