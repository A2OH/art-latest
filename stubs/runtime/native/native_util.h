/*
 * Copyright (C) 2017 The Android Open Source Project
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

// Patched native_util.h: tolerant RegisterNatives that skips methods
// not present in the core JARs (e.g. A15 runtime vs A11 core JARs).

#ifndef ART_RUNTIME_NATIVE_NATIVE_UTIL_H_
#define ART_RUNTIME_NATIVE_NATIVE_UTIL_H_

#include <jni.h>
#include <cstdio>

#include "android-base/logging.h"
#include "base/macros.h"
#include "nativehelper/scoped_local_ref.h"

namespace art HIDDEN {

ALWAYS_INLINE inline void RegisterNativeMethodsInternal(JNIEnv* env,
                                                        const char* jni_class_name,
                                                        const JNINativeMethod* methods,
                                                        jint method_count) {
  ScopedLocalRef<jclass> c(env, env->FindClass(jni_class_name));
  if (c.get() == nullptr) {
    // Class not found in core JARs -- clear exception and skip entirely
    fprintf(stderr, "[native_util] Class %s not found, skipping registration\n", jni_class_name);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return;
  }
  jint jni_result = env->RegisterNatives(c.get(), methods, method_count);
  if (jni_result == JNI_OK) {
    return;  // All methods registered successfully
  }
  // Bulk registration failed -- some methods in gMethods don't exist in the JAR.
  // Clear the exception and try registering methods one-by-one.
  if (env->ExceptionCheck()) env->ExceptionClear();
  fprintf(stderr, "[native_util] Bulk RegisterNatives for %s failed, trying one-by-one (%d methods)\n",
          jni_class_name, method_count);
  int registered = 0;
  for (jint i = 0; i < method_count; i++) {
    jint r = env->RegisterNatives(c.get(), &methods[i], 1);
    if (r == JNI_OK) {
      registered++;
    } else {
      fprintf(stderr, "[native_util]   skipping %s.%s%s (not in JAR)\n",
              jni_class_name, methods[i].name, methods[i].signature);
      if (env->ExceptionCheck()) env->ExceptionClear();
    }
  }
  fprintf(stderr, "[native_util] Registered %d/%d native methods for %s\n",
          registered, method_count, jni_class_name);
}

#define REGISTER_NATIVE_METHODS(jni_class_name) \
  RegisterNativeMethodsInternal(env, (jni_class_name), gMethods, arraysize(gMethods))

}  // namespace art

#endif  // ART_RUNTIME_NATIVE_NATIVE_UTIL_H_
