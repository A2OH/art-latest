// JNI entry point for libwestlake_art.so
// Runs our ART v114 runtime on a dedicated thread to avoid
// conflicting with the phone's ART v118 on the calling thread.

#include <jni.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
// Use write(STDERR_FILENO) which goes to logcat, not __android_log_print (our .so stubs it)
static void wlog(const char* fmt, ...) {
  char buf[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  write(STDERR_FILENO, buf, n);
  write(STDERR_FILENO, "\n", 1);
}
#define WLOG(...) wlog(__VA_ARGS__)
#include <string>
#include <vector>
#include <pthread.h>
#include <semaphore.h>

#include "runtime.h"
#include "thread.h"
#include "class_linker.h"
#include "mirror/string.h"
#include "mirror/class-inl.h"
#include "art_method-inl.h"
#include "gc/heap.h"
#include "scoped_thread_state_change-inl.h"

using namespace art;

static std::string JstringToString(JNIEnv* env, jstring jstr) {
  if (!jstr) return "";
  const char* c = env->GetStringUTFChars(jstr, nullptr);
  std::string s(c);
  env->ReleaseStringUTFChars(jstr, c);
  return s;
}

// Arguments for the engine thread
struct EngineArgs {
  std::string bootClassPath;
  std::string mainClass;
  int result;
  sem_t done;
};

// Engine thread: creates and runs our ART runtime
static void* engine_thread(void* arg) {
  auto* ea = static_cast<EngineArgs*>(arg);
  // Change to westlake dir so relative paths work for boot image + JARs
  chdir("/data/local/tmp/westlake");
  // Redirect stderr to a file so we can see ART's error messages
  int stderrFd = open("/data/data/com.westlake.host/cache/stderr.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
  if (stderrFd >= 0) { dup2(stderrFd, STDERR_FILENO); close(stderrFd); }
  WLOG("[WestlakeART] Engine thread started, cwd=/data/local/tmp/westlake main=%s\n", ea->mainClass.c_str());

  // Build runtime options
  RuntimeOptions options;
  options.push_back({"-Xbootclasspath:" + ea->bootClassPath, nullptr});
  // Boot image at 0x30000000 (free gap in app address space: 0x22000000-0x42000000)
  options.push_back({"-Ximage:/data/local/tmp/westlake/boot-img/boot.art", nullptr});
  options.push_back({"-Xnoimage-dex2oat", nullptr});
  options.push_back({"-Xgc:nonconcurrent", nullptr});
  options.push_back({"-Xms256m", nullptr});  // Large initial heap to avoid GC
  options.push_back({"-Xmx512m", nullptr});  // Large max heap
  options.push_back({"-Xusejit:false", nullptr});
  options.push_back({"-Xint", nullptr});

  // Create our ART runtime (on this NEW thread — no conflict with phone's ART)
  if (!Runtime::Create(options, false)) {
    ea->result = -1;
    sem_post(&ea->done);
    return nullptr;
  }

  Runtime* runtime = Runtime::Current();
  bool hasImage = runtime->GetHeap()->HasBootImageSpace();
  size_t numSpaces = runtime->GetHeap()->GetBootImageSpaces().size();

  if (!runtime->Start()) {
    WLOG("[WestlakeART] ERROR: Runtime::Start failed\n");
    ea->result = -2;
    sem_post(&ea->done);
    return nullptr;
  }

  WLOG("[WestlakeART] Runtime started OK\n");

  // Get JNI env — Runtime::Start attaches this thread as "main"
  JNIEnv* env = nullptr;
  JavaVM* jvm = reinterpret_cast<JavaVM*>(runtime->GetJavaVM());
  jvm->AttachCurrentThread(&env, nullptr);
  WLOG("[WestlakeART] JNI env = %p\n", env);

  // Apply critical bytecode patches (same as dalvikvm.cc InvokeMain)
  WLOG("[WestlakeART] Applying patches...\n");
  {
    // Patch Throwable.printStackTrace() → no-op (prevents infinite exception loops)
    jclass throwCls = env->FindClass("java/lang/Throwable");
    WLOG("[WestlakeART] Throwable class = %p\n", throwCls);
    if (throwCls) {
      // Mark the Java method as native and register our no-op
      art::ScopedObjectAccess soa(art::Thread::Current());
      art::ObjPtr<art::mirror::Class> tc =
          art::Runtime::Current()->GetClassLinker()->FindSystemClass(soa.Self(), "Ljava/lang/Throwable;");
      if (tc != nullptr) {
        for (art::ArtMethod& m : tc->GetDeclaredMethods(art::kRuntimePointerSize)) {
          if (strcmp(m.GetName(), "printStackTrace") == 0 &&
              m.GetSignature().ToString() == "()V" && !m.IsNative()) {
            m.SetAccessFlags(m.GetAccessFlags() | art::kAccNative);
            // Register via JNI
            static auto noop = +[](JNIEnv*, jobject) {};
            JNINativeMethod nm = {"printStackTrace", "()V", (void*)noop};
            env->RegisterNatives(throwCls, &nm, 1);
            WLOG("[WestlakeART] Patched Throwable.printStackTrace\n");
            break;
          }
        }
      }
      if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Patch TextUtils.formatSimple → native (prevents Build clinit StackOverflow)
    {
      art::ScopedObjectAccess soa(art::Thread::Current());
      art::ObjPtr<art::mirror::Class> txCls =
          art::Runtime::Current()->GetClassLinker()->FindSystemClass(
              soa.Self(), "Landroid/text/TextUtils;");
      if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
      if (txCls != nullptr) {
        for (art::ArtMethod& m : txCls->GetDeclaredMethods(art::kRuntimePointerSize)) {
          if (strcmp(m.GetName(), "formatSimple") == 0 &&
              m.GetSignature().ToString() ==
                  "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;" &&
              !m.IsNative()) {
            m.SetAccessFlags(m.GetAccessFlags() | art::kAccNative);
            // Simple C implementation that handles %s/%d/%x
            // formatSimple native — simple inline implementation
            static auto formatSimpleNative = +[](JNIEnv* e, jclass, jstring fmt, jobjectArray args) -> jstring {
              if (!fmt) return e->NewStringUTF("");
              const char* f = e->GetStringUTFChars(fmt, nullptr);
              if (!f) return fmt;
              int argc = args ? e->GetArrayLength(args) : 0;
              int ai = 0;
              char out[2048]; int pos = 0;
              for (const char* p = f; *p && pos < 2040; p++) {
                if (*p == '%' && *(p+1)) {
                  const char* s = p+1;
                  while (*s=='-'||*s=='+'||*s=='0'||*s==' '||*s=='#'||(*s>='0'&&*s<='9')||*s=='.') s++;
                  if (*s=='s' && ai<argc) {
                    jobject a = e->GetObjectArrayElement(args, ai++);
                    if (a) { const char* v = e->GetStringUTFChars((jstring)a, nullptr);
                      if (v) { int n=strlen(v); if(pos+n<2040){memcpy(out+pos,v,n);pos+=n;} e->ReleaseStringUTFChars((jstring)a,v); }
                    } else { memcpy(out+pos,"null",4); pos+=4; }
                    if (e->ExceptionCheck()) e->ExceptionClear();
                    p = s;
                  } else if ((*s=='d'||*s=='x'||*s=='X') && ai<argc) { ai++; out[pos++]='0'; p=s;
                  } else if (*s=='%') { out[pos++]='%'; p=s;
                  } else { if(ai<argc) ai++; p=s; }
                } else { out[pos++] = *p; }
              }
              out[pos] = 0;
              e->ReleaseStringUTFChars(fmt, f);
              if (e->ExceptionCheck()) e->ExceptionClear();
              return e->NewStringUTF(out);
            };
            m.SetEntryPointFromJni(reinterpret_cast<void*>(+formatSimpleNative));
            WLOG("[WestlakeART] Patched TextUtils.formatSimple\n");
            break;
          }
        }
      }
      if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
    }
  }

  // Pre-force critical framework classes to "initialized" status
  // This prevents clinit cascades that cause StackOverflow
  {
    art::ScopedObjectAccess soa(art::Thread::Current());
    const char* forceInitClasses[] = {
      "Landroid/os/Build;",
      "Landroid/os/Build$VERSION;",
      "Landroid/os/Build$VERSION_CODES;",
      "Landroid/os/LocaleList;",
      "Landroid/content/res/Configuration;",
      "Landroid/app/Activity;",
      "Landroid/app/ContextImpl;",
      "Landroid/app/ActivityThread;",
      "Landroid/app/LoadedApk;",
      "Landroid/app/Instrumentation;",
      "Landroid/app/Application;",
      "Landroid/os/Handler;",
      "Landroid/os/Looper;",
      "Landroid/os/Bundle;",
      "Landroid/os/Parcel;",
      "Landroid/content/Intent;",
      "Landroid/content/Context;",
      "Landroid/content/ContextWrapper;",
      "Landroid/view/ContextThemeWrapper;",
      "Landroid/content/res/Resources;",
      "Landroid/content/pm/ApplicationInfo;",
      "Landroid/content/pm/PackageManager;",
      "Landroid/view/Window;",
      "Landroid/view/WindowManager;",
      "Landroid/app/ResourcesManager;",
      "Landroid/text/TextUtils;",
      "Lsun/misc/Unsafe;",
      "Ljdk/internal/misc/Unsafe;",
      "Landroid/os/SystemProperties;",
      "Landroid/util/Log;",
      "Landroid/os/Parcelable;",
      "Landroid/app/WindowConfiguration;",
      "Landroid/graphics/Rect;",
      nullptr
    };
    int forced = 0;
    for (int i = 0; forceInitClasses[i]; i++) {
      art::ObjPtr<art::mirror::Class> cls =
          runtime->GetClassLinker()->FindSystemClass(soa.Self(), forceInitClasses[i]);
      if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
      if (cls != nullptr && !cls->IsInitialized()) {
        // Force class to initialized via EnsureInitialized (runs clinit but tolerance catches failures)
        art::StackHandleScope<1> hs(soa.Self());
        art::Handle<art::mirror::Class> h = hs.NewHandle(cls);
        runtime->GetClassLinker()->EnsureInitialized(soa.Self(), h, true, true);
        if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
        forced++;
      }
    }
    WLOG("[WestlakeART] Force-initialized %d framework classes\n", forced);
  }

  // Register helper natives for McdLoader
  {
    jclass loaderCls = env->FindClass("McdLoader");
    if (loaderCls) {
      static auto allocInstance = +[](JNIEnv* e, jclass, jclass target) -> jobject {
        return e->AllocObject(target);
      };
      static auto nativeLog = +[](JNIEnv* e, jclass, jstring msg) {
        if (!msg) return;
        const char* s = e->GetStringUTFChars(msg, nullptr);
        if (s) { write(STDERR_FILENO, s, strlen(s)); write(STDERR_FILENO, "\n", 1); e->ReleaseStringUTFChars(msg, s); }
      };
      JNINativeMethod nm1 = {"nativeAllocInstance", "(Ljava/lang/Class;)Ljava/lang/Object;", (void*)+allocInstance};
      JNINativeMethod nm2 = {"nativeLog", "(Ljava/lang/String;)V", (void*)+nativeLog};
      int rc1 = env->RegisterNatives(loaderCls, &nm1, 1);
      if (env->ExceptionCheck()) env->ExceptionClear();
      WLOG("[WestlakeART] RegisterNatives allocInstance: %d\n", rc1);
      int rc2 = env->RegisterNatives(loaderCls, &nm2, 1);
      if (env->ExceptionCheck()) env->ExceptionClear();
      WLOG("[WestlakeART] RegisterNatives nativeLog: %d\n", rc2);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }

  // Find and call the main class
  std::string jniName = ea->mainClass;
  for (char& c : jniName) if (c == '.') c = '/';

  jclass cls = env->FindClass(jniName.c_str());
  if (!cls) {
    WLOG("[WestlakeART] Class not found: %s\n", ea->mainClass.c_str());
    env->ExceptionClear();
    ea->result = -3;
    sem_post(&ea->done);
    return nullptr;
  }

  jmethodID mainMethod = env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V");
  if (!mainMethod) {
    WLOG("[WestlakeART] main() not found\n");
    env->ExceptionClear();
    ea->result = -4;
    sem_post(&ea->done);
    return nullptr;
  }

  jclass stringClass = env->FindClass("java/lang/String");
  jobjectArray args = env->NewObjectArray(0, stringClass, nullptr);

  // Write all state to log
  FILE* logf = fopen("/data/data/com.westlake.host/cache/engine.log", "w");
  if (logf) {
    fprintf(logf, "BOOT_IMAGE: loaded=%d spaces=%zu\n", hasImage, numSpaces);
    fprintf(logf, "RUNTIME: started env=%p\n", env);
    fprintf(logf, "CALLING_MAIN: %s\n", ea->mainClass.c_str());
    fflush(logf);
  }
  else { WLOG("Cannot open log file"); }

  WLOG("[WestlakeART] Calling %s.main()\n", ea->mainClass.c_str());
  env->CallStaticVoidMethod(cls, mainMethod, args);

  if (logf) { fprintf(logf, "MAIN_RETURNED\n"); fflush(logf); }

  if (env->ExceptionCheck()) {
    // Get exception details
    jthrowable exc = env->ExceptionOccurred();
    env->ExceptionClear();
    if (exc && logf) {
      jclass excCls = env->GetObjectClass(exc);
      jmethodID getName = env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
      jstring name = (jstring)env->CallObjectMethod(excCls, getName);
      const char* nameStr = name ? env->GetStringUTFChars(name, nullptr) : "?";
      jmethodID getMsg = env->GetMethodID(excCls, "getMessage", "()Ljava/lang/String;");
      jstring msg = (jstring)env->CallObjectMethod(exc, getMsg);
      const char* msgStr = msg ? env->GetStringUTFChars(msg, nullptr) : "(null)";
      fprintf(logf, "EXCEPTION: %s: %s\n", nameStr, msgStr);
      fflush(logf);
      if (name) env->ReleaseStringUTFChars(name, nameStr);
      if (msg) env->ReleaseStringUTFChars(msg, msgStr);
    }
    WLOG("[WestlakeART] Exception in main()");
    ea->result = 1;
  } else {
    WLOG("[WestlakeART] main() returned OK\n");
    ea->result = 0;
  }

  if (logf) { fprintf(logf, "ENGINE_DONE result=%d\n", ea->result); fflush(logf); fclose(logf); }
  sem_post(&ea->done);
  // Don't return — keep the engine thread alive to prevent runtime shutdown/GC crash
  // The host app manages the lifecycle.
  while (true) { sleep(3600); }
  return nullptr;
}

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  setenv("WESTLAKE_INPROCESS", "1", 1);
  WLOG("[WestlakeART] JNI_OnLoad (in-process, symbol-isolated)\n");
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_westlake_engine_WestlakeEngine_nativeLog(JNIEnv*, jclass, jstring msg) {
  // Can't use host_env for our strings — just use fprintf
  WLOG("[WestlakeART] (log call)\n");
}

JNIEXPORT jint JNICALL
Java_com_westlake_engine_WestlakeEngine_nativeStart(
    JNIEnv* host_env, jclass,
    jstring jBootClassPath, jstring jDexPaths, jstring jMainClass, jobjectArray jArgs) {

  // Extract strings from host JNI env (phone's ART)
  std::string bootClassPath = JstringToString(host_env, jBootClassPath);
  std::string dexPaths = JstringToString(host_env, jDexPaths);
  std::string mainClass = JstringToString(host_env, jMainClass);

  // Append dexPaths to bootClassPath if non-empty
  std::string fullBcp = bootClassPath;
  if (!dexPaths.empty()) {
    fullBcp += ":" + dexPaths;
  }

  WLOG("[WestlakeART] nativeStart: bcp=%s main=%s\n",
          fullBcp.c_str(), mainClass.c_str());

  // Launch on a SEPARATE thread so our Thread::Attach doesn't conflict
  // with the phone's ART on the calling thread.
  EngineArgs ea;
  ea.bootClassPath = fullBcp;
  ea.mainClass = mainClass;
  ea.result = -99;
  sem_init(&ea.done, 0, 0);

  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 32 * 1024 * 1024); // 32MB stack for deep class init chains
  int rc = pthread_create(&tid, &attr, engine_thread, &ea);
  pthread_attr_destroy(&attr);

  if (rc != 0) {
    WLOG("[WestlakeART] pthread_create failed: %d\n", rc);
    return -10;
  }

  // Wait for engine to finish
  sem_wait(&ea.done);
  sem_destroy(&ea.done);

  WLOG("[WestlakeART] Engine finished with result: %d\n", ea.result);
  return ea.result;
}

}  // extern "C"

// Thread::Current() is now provided inline via the gc/heap.h include chain
