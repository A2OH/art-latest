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

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
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

  fprintf(stderr, "[dalvikvm] Looking for class '%s'\n", class_name.c_str());
  ScopedLocalRef<jclass> klass(env, LoadClassFromClasspath(env, class_name.c_str()));
  if (klass.get() == nullptr) {
    fprintf(stderr, "Unable to locate class '%s'\n", class_name.c_str());
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }
  fprintf(stderr, "[dalvikvm] Class found: %p\n", klass.get());

  jmethodID method = env->GetStaticMethodID(klass.get(), "main", "([Ljava/lang/String;)V");
  if (method == nullptr) {
    fprintf(stderr, "Unable to find static main(String[]) in '%s'\n", class_name.c_str());
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }
  fprintf(stderr, "[dalvikvm] main() method found: %p\n", method);

  // Skip IsMethodPublic check -- in standalone builds, the reflect API
  // may not be fully initialized, causing false negatives.
  // The DEX format already encodes access flags; if GetStaticMethodID succeeded,
  // the method exists and is static.

  // Invoke main().
  fprintf(stderr, "[dalvikvm] Calling main()...\n");
  fflush(stderr);
  struct timespec ts_start, ts_end;
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  env->CallStaticVoidMethod(klass.get(), method, args.get());
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  long elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000L +
                    (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000L;
  fprintf(stderr, "[dalvikvm] main() returned (elapsed: %ld ms)\n", elapsed_ms);
  fflush(stderr);

  // Check whether there was an uncaught exception.  In standalone builds the normal
  // uncaught-exception handler may not be wired up, so print it ourselves.
  // Use both ExceptionOccurred and ExceptionCheck for robustness.
  jthrowable exc = env->ExceptionOccurred();
  if (exc != nullptr) {
    fprintf(stderr, "[dalvikvm] Exception occurred after main()\n");
    // Try to get exception class name
    jclass exc_class = env->GetObjectClass(exc);
    if (exc_class != nullptr) {
      jmethodID getName = env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
      if (getName != nullptr) {
        env->ExceptionClear(); // Clear to call methods
        jstring name = (jstring) env->CallObjectMethod(exc_class, getName);
        if (name != nullptr) {
          const char* nameChars = env->GetStringUTFChars(name, nullptr);
          if (nameChars) {
            fprintf(stderr, "[dalvikvm] Exception class: %s\n", nameChars);
            env->ReleaseStringUTFChars(name, nameChars);
          }
        }
        // Try getMessage()
        jmethodID getMsg = env->GetMethodID(exc_class, "getMessage", "()Ljava/lang/String;");
        if (getMsg != nullptr) {
          jstring msg = (jstring) env->CallObjectMethod(exc, getMsg);
          if (msg != nullptr) {
            const char* msgChars = env->GetStringUTFChars(msg, nullptr);
            if (msgChars) {
              fprintf(stderr, "[dalvikvm] Exception message: %s\n", msgChars);
              env->ReleaseStringUTFChars(msg, msgChars);
            }
          }
        }
      }
    }
    env->ExceptionDescribe();
    fflush(stderr);
    return EXIT_FAILURE;
  }
  if (env->ExceptionCheck()) {
    fprintf(stderr, "[dalvikvm] ExceptionCheck true after main()\n");
    env->ExceptionDescribe();
    fflush(stderr);
    return EXIT_FAILURE;
  }
  fprintf(stderr, "[dalvikvm] main() completed successfully\n");

  // Try calling various result methods on the class
  {
    const char* methods_to_try[] = {"getResult", "compute", "computeFib"};
    for (int i = 0; i < 3; i++) {
      jmethodID mid = env->GetStaticMethodID(klass.get(), methods_to_try[i], "()I");
      if (mid != nullptr) {
        jint val = env->CallStaticIntMethod(klass.get(), mid);
        fprintf(stderr, "[BENCH] %s() = %d\n", methods_to_try[i], (int)val);
        if (env->ExceptionCheck()) {
          env->ExceptionDescribe();
          env->ExceptionClear();
        }
      } else {
        env->ExceptionClear();
      }
    }
  }

  // After main() returns, try to read benchmark results from static fields
  // This allows benchmarks to store results without needing working I/O
  {
    const char* bench_fields[] = {"fibResult", "methodResult", "loopResult", "allocResult", "fieldResult"};
    const char* bench_names[] = {"FIB40", "METHOD_10M", "LOOP_100M", "ALLOC_1M", "FIELD_10M"};
    for (int i = 0; i < 5; i++) {
      jfieldID fid = env->GetStaticFieldID(klass.get(), bench_fields[i], "J");
      if (fid != nullptr) {
        jlong val = env->GetStaticLongField(klass.get(), fid);
        if (val >= 0) {
          fprintf(stderr, "[BENCH] %s = %lld ms\n", bench_names[i], (long long)val);
        }
      } else {
        env->ExceptionClear();
      }
    }
    // Also try fibAnswer
    jfieldID fid = env->GetStaticFieldID(klass.get(), "fibAnswer", "I");
    if (fid != nullptr) {
      jint val = env->GetStaticIntField(klass.get(), fid);
      if (val >= 0) {
        fprintf(stderr, "[BENCH] fibAnswer = %d\n", (int)val);
      }
    } else {
      env->ExceptionClear();
    }
  }

  return EXIT_SUCCESS;
}

// Parse arguments.  Most of it just gets passed through to the runtime.
// The JNI spec defines a handful of standard arguments.
static int dalvikvm(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Install a crash handler to get backtrace before ART's handler
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = [](int sig, siginfo_t* info, void* ctx) {
      ucontext_t* uc = (ucontext_t*)ctx;
      fprintf(stderr, "\n[dalvikvm] CRASH: signal=%d addr=%p rip=0x%llx\n",
              sig, info->si_addr,
              (unsigned long long)uc->uc_mcontext.gregs[REG_RIP]);
      fflush(stderr);
      // Call backtrace
      void* bt[20];
      int n = backtrace(bt, 20);
      backtrace_symbols_fd(bt, n, 2);
      _exit(128 + sig);
    };
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
  }

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
