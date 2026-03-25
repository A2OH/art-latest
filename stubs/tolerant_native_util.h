/*
 * Tolerant native_util.h replacement.
 * When RegisterNatives fails in bulk (some methods not in JAR),
 * falls back to one-by-one registration, skipping missing methods.
 */

#ifndef ART_TOLERANT_NATIVE_UTIL_H_
#define ART_TOLERANT_NATIVE_UTIL_H_

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
    LOG(WARNING) << "RegisterNatives: class " << jni_class_name << " not found, skipping";
    if (env->ExceptionCheck()) env->ExceptionClear();
    return;
  }
  jint jni_result = env->RegisterNatives(c.get(), methods, method_count);
  if (jni_result == JNI_OK) {
    return;
  }
  // Bulk registration failed. Try one-by-one.
  if (env->ExceptionCheck()) env->ExceptionClear();
  int registered = 0;
  for (jint i = 0; i < method_count; i++) {
    jint r = env->RegisterNatives(c.get(), &methods[i], 1);
    if (r == JNI_OK) {
      registered++;
    } else {
      LOG(WARNING) << "RegisterNatives: skipping " << jni_class_name
                   << "." << methods[i].name << methods[i].signature
                   << " (not in JAR)";
      if (env->ExceptionCheck()) env->ExceptionClear();
    }
  }
  LOG(INFO) << "RegisterNatives: " << jni_class_name << ": "
            << registered << "/" << method_count << " methods registered";
}

#define REGISTER_NATIVE_METHODS(jni_class_name) \
  RegisterNativeMethodsInternal(env, (jni_class_name), gMethods, arraysize(gMethods))

}  // namespace art

#endif  // ART_TOLERANT_NATIVE_UTIL_H_
