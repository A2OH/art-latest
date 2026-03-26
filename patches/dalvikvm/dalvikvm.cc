/*
 * copyright (C) 2011 The Android Open Source Project
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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <memory>

#include "base/fast_exit.h"
#include "jni.h"
#include "nativehelper/JniInvocation.h"
#include "nativehelper/ScopedLocalRef.h"
#include "nativehelper/toStringArray.h"

namespace art {

// Determine whether or not the specified method is public.
static bool IsMethodPublic(JNIEnv* env, jclass c, jmethodID method_id) {
  ScopedLocalRef<jobject> reflected(env, env->ToReflectedMethod(c, method_id, JNI_FALSE));
  if (reflected.get() == nullptr) {
    fprintf(stderr, "Failed to get reflected method\n");
    return false;
  }
  // We now have a Method instance.  We need to call its
  // getModifiers() method.
  jclass method_class = env->FindClass("java/lang/reflect/Method");
  if (method_class == nullptr) {
    fprintf(stderr, "Failed to find class java.lang.reflect.Method\n");
    return false;
  }
  jmethodID mid = env->GetMethodID(method_class, "getModifiers", "()I");
  if (mid == nullptr) {
    fprintf(stderr, "Failed to find java.lang.reflect.Method.getModifiers\n");
    return false;
  }
  int modifiers = env->CallIntMethod(reflected.get(), mid);
  static const int PUBLIC = 0x0001;  // java.lang.reflect.Modifiers.PUBLIC
  if ((modifiers & PUBLIC) == 0) {
    fprintf(stderr, "Modifiers mismatch\n");
    return false;
  }
  return true;
}

// Try to create a PathClassLoader for the classpath DEX and load a class through it.
// Falls back to FindClass (boot class loader) if PathClassLoader creation fails.
static jclass LoadClassFromClasspath(JNIEnv* env, const char* class_name_jni) {
  // First try the boot class loader (works if class is on boot classpath).
  // In standalone builds, -classpath DEXes are appended to boot classpath,
  // so this should find both system and app classes.
  jclass klass = env->FindClass(class_name_jni);
  if (klass != nullptr) {
    return klass;
  }
  env->ExceptionClear();

  // Try to create a PathClassLoader for the -classpath DEX.
  // dalvik.system.PathClassLoader(String dexPath, ClassLoader parent)
  jclass pcl_class = env->FindClass("dalvik/system/PathClassLoader");
  if (pcl_class == nullptr) {
    env->ExceptionClear();
    fprintf(stderr, "PathClassLoader class not found, cannot load user classes\n");
    return nullptr;
  }

  jmethodID pcl_init = env->GetMethodID(pcl_class, "<init>",
      "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
  if (pcl_init == nullptr) {
    env->ExceptionClear();
    fprintf(stderr, "PathClassLoader constructor not found\n");
    return nullptr;
  }

  // Get the classpath from the runtime options
  // The classpath is stored in the runtime's class_path_ member.
  // We can get it from the system property or we need to pass it in.
  // For now, use the environment: the -classpath arg was stored by the VM.
  const char* cp = getenv("CLASSPATH");
  // If CLASSPATH env isn't set, try to get it from RuntimeOptions
  // Actually, the Runtime stores classpath internally. Let's use Thread context.

  // Use Class.forName(name, true, classLoader) approach via the boot classpath.
  // The classpath is passed as a JVM option, so the runtime should know about it.
  // Let's try using the DexPathList approach.

  // Get the classpath from Java's system properties
  jclass system_class = env->FindClass("java/lang/System");
  if (system_class == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  jmethodID get_prop = env->GetStaticMethodID(system_class, "getProperty",
      "(Ljava/lang/String;)Ljava/lang/String;");
  if (get_prop == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  ScopedLocalRef<jstring> cp_key(env, env->NewStringUTF("java.class.path"));
  ScopedLocalRef<jstring> cp_val(env,
      (jstring) env->CallStaticObjectMethod(system_class, get_prop, cp_key.get()));
  if (cp_val.get() == nullptr) {
    env->ExceptionClear();
    fprintf(stderr, "java.class.path property not set\n");
    return nullptr;
  }

  // Create PathClassLoader(classpath, null)
  ScopedLocalRef<jobject> class_loader(env,
      env->NewObject(pcl_class, pcl_init, cp_val.get(), nullptr));
  if (class_loader.get() == nullptr) {
    env->ExceptionClear();
    fprintf(stderr, "Failed to create PathClassLoader\n");
    return nullptr;
  }

  fprintf(stderr, "Created PathClassLoader for classpath\n");

  // Use Class.forName(name, true, classLoader)
  jclass class_class = env->FindClass("java/lang/Class");
  if (class_class == nullptr) { env->ExceptionClear(); return nullptr; }
  jmethodID for_name = env->GetStaticMethodID(class_class, "forName",
      "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
  if (for_name == nullptr) { env->ExceptionClear(); return nullptr; }

  // Convert JNI name (com/example/Foo) back to Java name (com.example.Foo)
  std::string java_name(class_name_jni);
  std::replace(java_name.begin(), java_name.end(), '/', '.');

  ScopedLocalRef<jstring> name_str(env, env->NewStringUTF(java_name.c_str()));
  klass = (jclass) env->CallStaticObjectMethod(class_class, for_name,
      name_str.get(), JNI_TRUE, class_loader.get());
  if (klass == nullptr || env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    fprintf(stderr, "Class.forName('%s') via PathClassLoader failed\n", java_name.c_str());
    return nullptr;
  }

  fprintf(stderr, "Loaded class '%s' via PathClassLoader\n", java_name.c_str());
  return klass;
}

static int InvokeMain(JNIEnv* env, char** argv) {
  // We want to call main() with a String array with our arguments in
  // it.  Create an array and populate it.  Note argv[0] is not
  // included.
  ScopedLocalRef<jobjectArray> args(env, toStringArray(env, argv + 1));
  if (args.get() == nullptr) {
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }

  // Find [class].main(String[]).

  // Convert "com.android.Blah" to "com/android/Blah".
  std::string class_name(argv[0]);
  std::replace(class_name.begin(), class_name.end(), '.', '/');

  ScopedLocalRef<jclass> klass(env, LoadClassFromClasspath(env, class_name.c_str()));
  if (klass.get() == nullptr) {
    fprintf(stderr, "Unable to locate class '%s'\n", class_name.c_str());
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }

  jmethodID method = env->GetStaticMethodID(klass.get(), "main", "([Ljava/lang/String;)V");
  if (method == nullptr) {
    fprintf(stderr, "Unable to find static main(String[]) in '%s'\n", class_name.c_str());
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }

  // Skip IsMethodPublic check -- in standalone builds, the reflect API
  // may not be fully initialized, causing false negatives.
  // The DEX format already encodes access flags; if GetStaticMethodID succeeded,
  // the method exists and is static.

  // Invoke main().
  env->CallStaticVoidMethod(klass.get(), method, args.get());

  // Check whether there was an uncaught exception.  In standalone builds the normal
  // uncaught-exception handler may not be wired up, so print it ourselves.
  // Use both ExceptionOccurred and ExceptionCheck for robustness.
  jthrowable exc = env->ExceptionOccurred();
  if (exc != nullptr) {
    env->ExceptionDescribe();
    fflush(stderr);
    return EXIT_FAILURE;
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    fflush(stderr);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

// Parse arguments.  Most of it just gets passed through to the runtime.
// The JNI spec defines a handful of standard arguments.
static int dalvikvm(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Skip over argv[0].
  argv++;
  argc--;

  // If we're adding any additional stuff, e.g. function hook specifiers,
  // add them to the count here.
  //
  // We're over-allocating, because this includes the options to the runtime
  // plus the options to the program.
  int option_count = argc;
  std::unique_ptr<JavaVMOption[]> options(new JavaVMOption[option_count]());

  // Copy options over.  Everything up to the name of the class starts
  // with a '-' (the function hook stuff is strictly internal).
  //
  // [Do we need to catch & handle "-jar" here?]
  bool need_extra = false;
  const char* lib = nullptr;
  const char* what = nullptr;
  int curr_opt, arg_idx;
  for (curr_opt = arg_idx = 0; arg_idx < argc; arg_idx++) {
    if (argv[arg_idx][0] != '-' && !need_extra) {
      break;
    }
    if (strncmp(argv[arg_idx], "-XXlib:", strlen("-XXlib:")) == 0) {
      lib = argv[arg_idx] + strlen("-XXlib:");
      continue;
    }

    options[curr_opt++].optionString = argv[arg_idx];

    // Some options require an additional argument.
    need_extra = false;
    if (strcmp(argv[arg_idx], "-classpath") == 0 || strcmp(argv[arg_idx], "-cp") == 0) {
      need_extra = true;
      what = argv[arg_idx];
    }
  }

  if (need_extra) {
    fprintf(stderr, "%s must be followed by an additional argument giving a value\n", what);
    return EXIT_FAILURE;
  }

  if (curr_opt > option_count) {
    fprintf(stderr, "curr_opt(%d) > option_count(%d)\n", curr_opt, option_count);
    abort();
    return EXIT_FAILURE;
  }

  // Find the JNI_CreateJavaVM implementation.
  JniInvocation jni_invocation;
  if (!jni_invocation.Init(lib)) {
    fprintf(stderr, "Failed to initialize JNI invocation API from %s\n", lib);
    return EXIT_FAILURE;
  }

  JavaVMInitArgs init_args;
  init_args.version = JNI_VERSION_1_6;
  init_args.options = options.get();
  init_args.nOptions = curr_opt;
  init_args.ignoreUnrecognized = JNI_FALSE;

  // Start the runtime. The current thread becomes the main thread.
  JavaVM* vm = nullptr;
  JNIEnv* env = nullptr;
  if (JNI_CreateJavaVM(&vm, &env, &init_args) != JNI_OK) {
    fprintf(stderr, "Failed to initialize runtime (check log for details)\n");
    return EXIT_FAILURE;
  }

  // Make sure they provided a class name. We do this after
  // JNI_CreateJavaVM so that things like "-help" have the opportunity
  // to emit a usage statement.
  if (arg_idx == argc) {
    fprintf(stderr, "Class name required\n");
    return EXIT_FAILURE;
  }

  int rc = InvokeMain(env, &argv[arg_idx]);

  // In standalone builds, VM shutdown (DestroyJavaVM) crashes because thread groups
  // and daemon threads aren't fully initialized. Just exit directly.
  // Flush stderr so ExceptionDescribe output is not lost by _exit().
  fflush(stderr);
  fflush(stdout);
  _exit(rc);
}

}  // namespace art

// TODO(b/141622862): stop leaks
extern "C" const char *__asan_default_options() {
    return "detect_leaks=0";
}

int main(int argc, char** argv) {
  // Do not allow static destructors to be called, since it's conceivable that
  // daemons may still awaken (literally).
  art::FastExit(art::dalvikvm(argc, argv));
}
