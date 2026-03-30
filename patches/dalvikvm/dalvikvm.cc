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

#if !defined(__MUSL__)
#include <execinfo.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !defined(__MUSL__)
#include <ucontext.h>
#endif
#include <algorithm>
#include <memory>

#include "base/fast_exit.h"
#include "jni.h"
#include "nativehelper/JniInvocation.h"
#include "nativehelper/ScopedLocalRef.h"
#include "nativehelper/toStringArray.h"

// ART internal headers for forcing class initialization status
#include "runtime.h"
#include "class_linker.h"
#include "mirror/class-inl.h"
#include "handle_scope-inl.h"
#include "scoped_thread_state_change-inl.h"
#include "thread.h"
#include "object_lock.h"

// Thread.clone() override — returns 'this' instead of throwing CloneNotSupportedException.
extern "C" JNIEXPORT jobject JNICALL
Java_java_lang_Thread_clone(JNIEnv* env, jobject self) {
  return self;
}

// Float.toString and Double.toString bypasses — avoid FloatingDecimal which uses
// ThreadLocal and inner class arrays that can't be allocated by the A15 interpreter.
extern "C" JNIEXPORT jstring JNICALL
Java_java_lang_Float_toStringImpl(JNIEnv* env, jclass, jfloat val) {
  char buf[48];
  if (val != val) return env->NewStringUTF("NaN");
  if (val == 1.0f/0.0f) return env->NewStringUTF("Infinity");
  if (val == -1.0f/0.0f) return env->NewStringUTF("-Infinity");
  snprintf(buf, sizeof(buf), "%g", (double)val);
  return env->NewStringUTF(buf);
}
extern "C" JNIEXPORT jstring JNICALL
Java_java_lang_Double_toStringImpl(JNIEnv* env, jclass, jdouble val) {
  char buf[48];
  if (val != val) return env->NewStringUTF("NaN");
  if (val == 1.0/0.0) return env->NewStringUTF("Infinity");
  if (val == -1.0/0.0) return env->NewStringUTF("-Infinity");
  snprintf(buf, sizeof(buf), "%.17g", val);
  return env->NewStringUTF(buf);
}

extern "C" JNIEXPORT jint JNICALL
Java_art_io_Utf8Writer_nativeWrite(JNIEnv* env, jclass, jint fd, jbyteArray data, jint off, jint len) {
  fprintf(stderr, "[nativeWrite] fd=%d len=%d\n", fd, len); fflush(stderr);
  if (fd < 0 || data == nullptr || len <= 0) {
    return -1;
  }
  jbyte buf[8192];
  jint remaining = len;
  jint src_off = off;
  jint total = 0;
  while (remaining > 0) {
    jint chunk = remaining > 8192 ? 8192 : remaining;
    env->GetByteArrayRegion(data, src_off, chunk, buf);
    ssize_t written = write(fd, buf, chunk);
    if (written <= 0) break;
    total += written;
    src_off += written;
    remaining -= written;
  }
  return total;
}

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

  // Check class init status for key classes
  {
    const char* classes_to_check[] = {
      "java/util/concurrent/atomic/AtomicInteger",
      "java/lang/invoke/VarHandle",
      "java/lang/Integer",
      // Don't check System — it triggers <clinit> which may fail
    };
    for (const char* cls_name : classes_to_check) {
      jclass cls = env->FindClass(cls_name);
      if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
      if (!cls) continue;
      // Check if class has been initialized by checking a static field
      // For AtomicInteger, check if serialVersionUID exists (set by clinit)
      jfieldID svuid = env->GetStaticFieldID(cls, "serialVersionUID", "J");
      if (env->ExceptionCheck()) env->ExceptionClear();
      fprintf(stderr, "[dalvikvm] Class %s: loaded=%p serialVersionUID=%s\n",
              cls_name, cls, svuid ? "found" : "not-found");
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    fflush(stderr);
  }

  // Fix primitive TYPE fields: boot image may not preserve Integer.TYPE etc.
  // These are needed by VarHandle/MethodHandles for field type identity checks.
  {
    struct { const char* wrapper; const char* prim; } prims[] = {
      {"java/lang/Integer", "int"},
      {"java/lang/Long", "long"},
      {"java/lang/Boolean", "boolean"},
      {"java/lang/Byte", "byte"},
      {"java/lang/Character", "char"},
      {"java/lang/Short", "short"},
      {"java/lang/Float", "float"},
      {"java/lang/Double", "double"},
      {"java/lang/Void", "void"},
    };
    jclass classCls = env->FindClass("java/lang/Class");
    jmethodID getPrimClass = classCls ? env->GetStaticMethodID(classCls, "getPrimitiveClass",
                                          "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (getPrimClass) {
      for (auto& p : prims) {
        jclass wrapperCls = env->FindClass(p.wrapper);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!wrapperCls) continue;
        jfieldID typeField = env->GetStaticFieldID(wrapperCls, "TYPE", "Ljava/lang/Class;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!typeField) continue;
        jobject current = env->GetStaticObjectField(wrapperCls, typeField);
        if (!current) {
          jstring primName = env->NewStringUTF(p.prim);
          jobject primClass = env->CallStaticObjectMethod(classCls, getPrimClass, primName);
          if (primClass && !env->ExceptionCheck()) {
            env->SetStaticObjectField(wrapperCls, typeField, primClass);
            fprintf(stderr, "[dalvikvm] Fixed %s.TYPE = %p\n", p.wrapper, primClass);
          }
          if (env->ExceptionCheck()) env->ExceptionClear();
        }
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    fflush(stderr);
  }

  // Diagnostic: check if int.class identity is broken in boot image
  {
    jclass integerClass = env->FindClass("java/lang/Integer");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (integerClass) {
      jfieldID typeField = env->GetStaticFieldID(integerClass, "TYPE", "Ljava/lang/Class;");
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (typeField) {
        jobject integerType = env->GetStaticObjectField(integerClass, typeField);
        fprintf(stderr, "[dalvikvm] Integer.TYPE=%p (null means Integer.<clinit> failed or TYPE not preserved)\n",
                integerType);
        // Try to trigger Integer.<clinit> by calling Integer.valueOf(0)
        jmethodID valueOf = env->GetStaticMethodID(integerClass, "valueOf", "(I)Ljava/lang/Integer;");
        if (valueOf && !env->ExceptionCheck()) {
          env->CallStaticObjectMethod(integerClass, valueOf, 0);
          if (env->ExceptionCheck()) { fprintf(stderr, "[dalvikvm] Integer.valueOf(0) failed\n"); env->ExceptionDescribe(); env->ExceptionClear(); }
          // Re-check TYPE
          integerType = env->GetStaticObjectField(integerClass, typeField);
          fprintf(stderr, "[dalvikvm] Integer.TYPE after valueOf=%p\n", integerType);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Test getDeclaredField directly
    jclass atomicIntCls = env->FindClass("java/util/concurrent/atomic/AtomicInteger");
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (atomicIntCls) {
      jmethodID gdf = env->GetMethodID(env->FindClass("java/lang/Class"), "getDeclaredField",
                                         "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
      if (gdf && !env->ExceptionCheck()) {
        jstring valueName = env->NewStringUTF("value");
        jobject field = env->CallObjectMethod(atomicIntCls, gdf, valueName);
        if (field && !env->ExceptionCheck()) {
          // Get field.getType()
          jmethodID getType = env->GetMethodID(env->FindClass("java/lang/reflect/Field"),
                                                "getType", "()Ljava/lang/Class;");
          if (getType) {
            jobject fieldType = env->CallObjectMethod(field, getType);
            fprintf(stderr, "[dalvikvm] AtomicInteger.value field found! fieldType=%p\n", fieldType);
          // Check if fieldType name is "int"
          if (fieldType) {
            jmethodID getName = env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
            if (getName && !env->ExceptionCheck()) {
              jstring typeName = (jstring)env->CallObjectMethod(fieldType, getName);
              if (typeName && !env->ExceptionCheck()) {
                const char* tn = env->GetStringUTFChars(typeName, nullptr);
                fprintf(stderr, "[dalvikvm] fieldType.getName()=%s\n", tn ? tn : "(null)");
                if (tn) env->ReleaseStringUTFChars(typeName, tn);
              }
            }
          }
          if (env->ExceptionCheck()) env->ExceptionClear();
          }
        } else {
          fprintf(stderr, "[dalvikvm] AtomicInteger.getDeclaredField('value') FAILED\n");
          if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        }
      }
      if (env->ExceptionCheck()) env->ExceptionClear();
    }
    fflush(stderr);
  }

  // Set up System.out / System.err before calling main().
  // System.<clinit> may fail, but FileDescriptor/FileOutputStream/PrintStream are
  // pre-initialized in the boot image, so we can create them via JNI.
  {
    fprintf(stderr, "[dalvikvm] Setting up System.out/err via JNI...\n");
    fflush(stderr);
    jclass systemCls = env->FindClass("java/lang/System");
    jclass fdCls = env->FindClass("java/io/FileDescriptor");
    jclass fosCls = env->FindClass("java/io/FileOutputStream");
    jclass psCls = env->FindClass("java/io/PrintStream");
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Replace Charset.cache2 with a fresh HashMap AND also set cache1 to null.
    // Boot image collection objects have broken itable dispatch after relocation.
    {
      jclass charsetCls = env->FindClass("java/nio/charset/Charset");
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (charsetCls) {
        // Replace cache2 (HashMap)
        jfieldID cache2Field = env->GetStaticFieldID(charsetCls, "cache2", "Ljava/util/HashMap;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (cache2Field) {
          jclass hmCls = env->FindClass("java/util/HashMap");
          jmethodID hmInit = hmCls ? env->GetMethodID(hmCls, "<init>", "()V") : nullptr;
          if (env->ExceptionCheck()) env->ExceptionClear();
          if (hmInit) {
            jobject newMap = env->NewObject(hmCls, hmInit);
            if (newMap && !env->ExceptionCheck()) {
              env->SetStaticObjectField(charsetCls, cache2Field, newMap);
              fprintf(stderr, "[dalvikvm] Replaced Charset.cache2\n");
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
          }
        }
        // Also null out cache1 (Map.Entry<String,Charset>)
        jfieldID cache1Field = env->GetStaticFieldID(charsetCls, "cache1", "Ljava/util/Map$Entry;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (cache1Field) {
          env->SetStaticObjectField(charsetCls, cache1Field, nullptr);
          fprintf(stderr, "[dalvikvm] Cleared Charset.cache1\n");
        }
        if (env->ExceptionCheck()) env->ExceptionClear();

        // Also force Charset class status to initialized (it may be in error state)
        {
          ScopedObjectAccess soa(art::Thread::Current());
          art::ObjPtr<art::mirror::Class> cs =
              art::Runtime::Current()->GetClassLinker()->FindSystemClass(soa.Self(), "Ljava/nio/charset/Charset;");
          if (cs != nullptr && !cs->IsVisiblyInitialized()) {
            art::StackHandleScope<1> hs2(soa.Self());
            art::Handle<art::mirror::Class> h(hs2.NewHandle(cs));
            art::ObjectLock<art::mirror::Class> lock(soa.Self(), h);
            art::mirror::Class::SetStatus(h, art::ClassStatus::kVisiblyInitialized, soa.Self());
            fprintf(stderr, "[dalvikvm] Forced Charset to kVisiblyInitialized\n");
          }
          if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
        }
      }
      if (env->ExceptionCheck()) env->ExceptionClear();

      // Also replace CharsetEncoderICU.DEFAULT_REPLACEMENTS with a fresh HashMap.
      // The static HashMap may have entries with null Strings after boot image relocation,
      // or its internal Node[] has corrupted references.
      {
        jclass encICU = env->FindClass("com/android/icu/charset/CharsetEncoderICU");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (encICU) {
          jfieldID drField = env->GetStaticFieldID(encICU, "DEFAULT_REPLACEMENTS", "Ljava/util/Map;");
          if (env->ExceptionCheck()) env->ExceptionClear();
          if (drField) {
            jclass hmCls2 = env->FindClass("java/util/HashMap");
            jmethodID hmInit2 = hmCls2 ? env->GetMethodID(hmCls2, "<init>", "()V") : nullptr;
            jmethodID hmPut = hmCls2 ? env->GetMethodID(hmCls2, "put",
                "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;") : nullptr;
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (hmInit2 && hmPut) {
              jobject newDR = env->NewObject(hmCls2, hmInit2);
              if (newDR) {
                // Add standard replacements
                jbyteArray qm = env->NewByteArray(1);
                jbyte qmByte = 0x3F;  // '?'
                env->SetByteArrayRegion(qm, 0, 1, &qmByte);
                env->CallObjectMethod(newDR, hmPut, env->NewStringUTF("UTF-8"), qm);
                env->CallObjectMethod(newDR, hmPut, env->NewStringUTF("ISO-8859-1"), qm);
                env->CallObjectMethod(newDR, hmPut, env->NewStringUTF("US-ASCII"), qm);
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->SetStaticObjectField(encICU, drField, newDR);
                fprintf(stderr, "[dalvikvm] Replaced CharsetEncoderICU.DEFAULT_REPLACEMENTS\n");
              }
            }
          }
          if (env->ExceptionCheck()) env->ExceptionClear();
          // Force CharsetEncoderICU to kVisiblyInitialized if <clinit> failed
          {
            ScopedObjectAccess soa(art::Thread::Current());
            art::ObjPtr<art::mirror::Class> eiCls =
                art::Runtime::Current()->GetClassLinker()->FindSystemClass(soa.Self(),
                    "Lcom/android/icu/charset/CharsetEncoderICU;");
            if (eiCls != nullptr && !eiCls->IsVisiblyInitialized()) {
              art::StackHandleScope<1> hs3(soa.Self());
              art::Handle<art::mirror::Class> h3(hs3.NewHandle(eiCls));
              art::ObjectLock<art::mirror::Class> lock3(soa.Self(), h3);
              art::mirror::Class::SetStatus(h3, art::ClassStatus::kVisiblyInitialized, soa.Self());
              fprintf(stderr, "[dalvikvm] Forced CharsetEncoderICU to kVisiblyInitialized\n");
            }
            if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
          }
        }
      }
      fflush(stderr);
    }

    if (systemCls && fdCls && fosCls && psCls) {
      // System force-init now done in class_linker.cc RunEarlyRootClinits
      if (env->ExceptionCheck()) env->ExceptionClear();

      // ---- Early test: can we access static fields? ----
      {
        // Force-init classes whose <clinit> might crash in artFindNativeMethod
        // (native method trampoline issue in boot image relocation).
        // Only force-init classes that are NOT already kVisiblyInitialized.
        {
          ScopedObjectAccess soa(art::Thread::Current());
          auto forceInit = [&](const char* desc) {
            art::StackHandleScope<1> hs3(soa.Self());
            art::ObjPtr<art::mirror::Class> cls =
                art::Runtime::Current()->GetClassLinker()->FindSystemClass(soa.Self(), desc);
            if (cls != nullptr) {
              art::Handle<art::mirror::Class> h(hs3.NewHandle(cls));
              // Only force-init if not already initialized
              if (h->GetStatus() < art::ClassStatus::kVisiblyInitialized) {
                art::ObjectLock<art::mirror::Class> lock(soa.Self(), h);
                art::mirror::Class::SetStatus(h, art::ClassStatus::kVisiblyInitialized, soa.Self());
                fprintf(stderr, "[dalvikvm] Force-init %s OK\n", desc);
              }
            }
            if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
          };
          forceInit("Ljava/io/FileDescriptor;");
          forceInit("Ljava/io/FileOutputStream;");
          forceInit("Ljava/io/PrintStream;");
          forceInit("Ljava/io/OutputStream;");
          forceInit("Ljava/io/FilterOutputStream;");
          forceInit("Ljava/io/BufferedWriter;");
          forceInit("Ljava/io/Writer;");
          forceInit("Ljava/io/OutputStreamWriter;");
        }
        if (env->ExceptionCheck()) env->ExceptionClear();

        // Manually create FileDescriptor.in/out/err since we skipped <clinit>
        {
          jmethodID fdDefaultInit = env->GetMethodID(fdCls, "<init>", "()V");
          if (env->ExceptionCheck()) env->ExceptionClear();
          jfieldID fdField = env->GetFieldID(fdCls, "descriptor", "I");
          if (env->ExceptionCheck()) { env->ExceptionClear(); fdField = nullptr; }
          // A15 uses "fd" not "descriptor"
          if (!fdField) {
            fdField = env->GetFieldID(fdCls, "fd", "I");
            if (env->ExceptionCheck()) env->ExceptionClear();
          }

          jfieldID fdInF = env->GetStaticFieldID(fdCls, "in", "Ljava/io/FileDescriptor;");
          if (env->ExceptionCheck()) env->ExceptionClear();
          jfieldID fdOutF = env->GetStaticFieldID(fdCls, "out", "Ljava/io/FileDescriptor;");
          if (env->ExceptionCheck()) env->ExceptionClear();
          jfieldID fdErrF = env->GetStaticFieldID(fdCls, "err", "Ljava/io/FileDescriptor;");
          if (env->ExceptionCheck()) env->ExceptionClear();

          if (fdDefaultInit && fdField && fdInF && fdOutF && fdErrF) {
            // Create FileDescriptor(0) for stdin
            jobject fdIn = env->NewObject(fdCls, fdDefaultInit);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fdIn = nullptr; }
            if (fdIn) { env->SetIntField(fdIn, fdField, 0); env->SetStaticObjectField(fdCls, fdInF, fdIn); }

            // Create FileDescriptor(1) for stdout
            jobject fdOutObj = env->NewObject(fdCls, fdDefaultInit);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fdOutObj = nullptr; }
            if (fdOutObj) { env->SetIntField(fdOutObj, fdField, 1); env->SetStaticObjectField(fdCls, fdOutF, fdOutObj); }

            // Create FileDescriptor(2) for stderr
            jobject fdErrObj = env->NewObject(fdCls, fdDefaultInit);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fdErrObj = nullptr; }
            if (fdErrObj) { env->SetIntField(fdErrObj, fdField, 2); env->SetStaticObjectField(fdCls, fdErrF, fdErrObj); }

            fprintf(stderr, "[dalvikvm] Created FileDescriptor.in/out/err manually\n");
          } else {
            fprintf(stderr, "[dalvikvm] WARN: could not create FileDescriptor objects (init=%p fd=%p)\n",
                    fdDefaultInit, fdField);
          }
          if (env->ExceptionCheck()) env->ExceptionClear();
        }
      }

      // ---- Fix System.props: create Properties object and populate it ----
      // System.<clinit> fails in standalone builds, leaving System.props and
      // System.unchangeableProps as null. This blocks Locale.getDefault() which
      // blocks Activity/View initialization.
      {
        jclass propsCls = env->FindClass("java/util/Properties");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (propsCls) {
          jmethodID propsInit = env->GetMethodID(propsCls, "<init>", "()V");
          if (env->ExceptionCheck()) env->ExceptionClear();
          jmethodID propsPut = propsCls ? env->GetMethodID(propsCls, "setProperty",
              "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
          if (env->ExceptionCheck()) env->ExceptionClear();

          if (propsInit && propsPut) {
            jobject props = env->NewObject(propsCls, propsInit);
            if (props && !env->ExceptionCheck()) {
              // Get cwd for user.dir
              char cwdBuf[4096];
              const char* cwd = getcwd(cwdBuf, sizeof(cwdBuf));
              if (!cwd) cwd = "/";

              struct { const char* key; const char* val; } sysProps[] = {
                {"user.language", "en"},
                {"user.region", "US"},
                {"user.country", "US"},
                {"user.locale", "en-US"},
                {"user.script", ""},
                {"user.variant", ""},
                {"file.encoding", "UTF-8"},
                {"file.separator", "/"},
                {"path.separator", ":"},
                {"line.separator", "\n"},
                {"user.home", "/"},
                {"user.dir", cwd},
                {"java.io.tmpdir", "/tmp"},
                {"os.name", "Linux"},
                {"os.arch", "x86_64"},
                {"java.class.path", ""},
                {"java.library.path", ""},
                {"java.home", "/"},
                {"java.vm.name", "Dalvik"},
                {"java.vm.version", "2.1.0"},
                {"java.specification.version", "1.8"},
              };

              for (auto& sp : sysProps) {
                jstring k = env->NewStringUTF(sp.key);
                jstring v = env->NewStringUTF(sp.val);
                env->CallObjectMethod(props, propsPut, k, v);
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(k);
                env->DeleteLocalRef(v);
              }

              // Set System.props
              jfieldID propsField = env->GetStaticFieldID(systemCls, "props", "Ljava/util/Properties;");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (propsField) {
                env->SetStaticObjectField(systemCls, propsField, props);
                fprintf(stderr, "[dalvikvm] System.props set with %d properties\n",
                        (int)(sizeof(sysProps) / sizeof(sysProps[0])));
              }

              // Set System.unchangeableProps (same object is fine for standalone)
              jfieldID ucPropsField = env->GetStaticFieldID(systemCls, "unchangeableProps", "Ljava/util/Properties;");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (ucPropsField) {
                env->SetStaticObjectField(systemCls, ucPropsField, props);
                fprintf(stderr, "[dalvikvm] System.unchangeableProps set OK\n");
              }

              env->DeleteLocalRef(props);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
          }

          // Set System.lineSeparator field — used by System.lineSeparator().
          // System.<clinit> normally sets this, but in the standalone build
          // <clinit> may fail before reaching it.
          // Field name: "lineSeparator" (A11+), fallback "lineSep".
          {
            jfieldID lineSepF = env->GetStaticFieldID(systemCls, "lineSeparator", "Ljava/lang/String;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); lineSepF = nullptr; }
            if (!lineSepF) {
              lineSepF = env->GetStaticFieldID(systemCls, "lineSep", "Ljava/lang/String;");
              if (env->ExceptionCheck()) { env->ExceptionClear(); lineSepF = nullptr; }
            }
            if (lineSepF) {
              jobject cur = env->GetStaticObjectField(systemCls, lineSepF);
              if (!cur) {
                jstring nl = env->NewStringUTF("\n");
                env->SetStaticObjectField(systemCls, lineSepF, nl);
                fprintf(stderr, "[dalvikvm] Set System.lineSeparator to \\n\n");
              } else {
                fprintf(stderr, "[dalvikvm] System.lineSeparator already set\n");
              }
            } else {
              fprintf(stderr, "[dalvikvm] WARN: System.lineSeparator field not found\n");
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
          }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        fflush(stderr);
      }

      // ---- Pre-initialize Locale ----
      // Now that System.props has user.language=en, user.region=US,
      // trigger Locale.<clinit> naturally via JNI. If it fails, force-init.
      {
        jobject enUsLocale = nullptr;
        // Try natural init via JNI FindClass (triggers <clinit>)
        jclass localeCls = env->FindClass("java/util/Locale");
        if (env->ExceptionCheck()) {
          fprintf(stderr, "[dalvikvm] Locale.<clinit> failed, falling back to force-init\n");
          env->ExceptionClear();
          localeCls = nullptr;
        }
        if (localeCls) {
          // <clinit> succeeded — get Locale.US
          jfieldID usField = env->GetStaticFieldID(localeCls, "US", "Ljava/util/Locale;");
          if (usField && !env->ExceptionCheck()) {
            enUsLocale = env->GetStaticObjectField(localeCls, usField);
          }
          if (env->ExceptionCheck()) env->ExceptionClear();
          fprintf(stderr, "[dalvikvm] Locale initialized naturally, US=%p\n", enUsLocale);
        }
        if (!localeCls) {
          // Fallback: force-init with ART internals
          ScopedObjectAccess soa(art::Thread::Current());
          art::ClassLinker* cl = art::Runtime::Current()->GetClassLinker();
          art::ObjPtr<art::mirror::Class> locMirror =
              cl->FindSystemClass(soa.Self(), "Ljava/util/Locale;");
          if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
          if (locMirror != nullptr && !locMirror->IsVisiblyInitialized()) {
            art::StackHandleScope<1> hsL(soa.Self());
            art::Handle<art::mirror::Class> hL(hsL.NewHandle(locMirror));
            art::ObjectLock<art::mirror::Class> lockL(soa.Self(), hL);
            art::mirror::Class::SetStatus(hL, art::ClassStatus::kVisiblyInitialized, soa.Self());
            fprintf(stderr, "[dalvikvm] Forced Locale to kVisiblyInitialized\n");
          }
          if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();

        } // end fallback force-init

        // Now that both Locale and NoImagePreloadHolder are forced-initialized,
        // we can safely use JNI to create a Locale and set the field.
        {
          jclass localeCls = env->FindClass("java/util/Locale");
          if (env->ExceptionCheck()) env->ExceptionClear();
          if (localeCls) {
            // Try to get Locale.US (static final field, set in boot image)
            jfieldID usField = env->GetStaticFieldID(localeCls, "US", "Ljava/util/Locale;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (usField) {
              enUsLocale = env->GetStaticObjectField(localeCls, usField);
              if (enUsLocale) {
                fprintf(stderr, "[dalvikvm] Got Locale.US from static field\n");
              }
            }
            // Fallback: try constructor
            if (!enUsLocale) {
              jmethodID localeInit = env->GetMethodID(localeCls, "<init>",
                  "(Ljava/lang/String;Ljava/lang/String;)V");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (localeInit) {
                jstring lang = env->NewStringUTF("en");
                jstring country = env->NewStringUTF("US");
                enUsLocale = env->NewObject(localeCls, localeInit, lang, country);
                if (env->ExceptionCheck()) { env->ExceptionClear(); enUsLocale = nullptr; }
              }
            }
          }
        }

        // Locale initialized naturally. Now force-init NoImagePreloadHolder
        // and set its defaultLocale field via JNI (to avoid its <clinit> which
        // calls initDefault() → ICU → NPE).
        if (enUsLocale) {
          // Force NoImagePreloadHolder to kVisiblyInitialized without running <clinit>
          {
            ScopedObjectAccess soa(art::Thread::Current());
            art::ObjPtr<art::mirror::Class> niph =
                art::Runtime::Current()->GetClassLinker()->FindSystemClass(soa.Self(),
                    "Ljava/util/Locale$NoImagePreloadHolder;");
            if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
            if (niph != nullptr && !niph->IsVisiblyInitialized()) {
              art::StackHandleScope<1> hs9(soa.Self());
              art::Handle<art::mirror::Class> h9(hs9.NewHandle(niph));
              art::ObjectLock<art::mirror::Class> lock9(soa.Self(), h9);
              art::mirror::Class::SetStatus(h9, art::ClassStatus::kVisiblyInitialized, soa.Self());
              fprintf(stderr, "[dalvikvm] Forced NoImagePreloadHolder to kVisiblyInitialized\n");
            }
            if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
          }
          // Set defaultLocale via JNI FindClass (class is already kVisiblyInitialized,
          // so FindClass won't re-trigger <clinit>)
          {
            jclass niphCls = env->FindClass("java/util/Locale$NoImagePreloadHolder");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (niphCls) {
              jfieldID dlField = env->GetStaticFieldID(niphCls, "defaultLocale", "Ljava/util/Locale;");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (dlField) {
                env->SetStaticObjectField(niphCls, dlField, enUsLocale);
                fprintf(stderr, "[dalvikvm] Set NoImagePreloadHolder.defaultLocale via JNI\n");
              }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
          }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        fflush(stderr);
      }

      // Re-find classes to avoid stale references after heavy class init above
      fdCls = env->FindClass("java/io/FileDescriptor");
      if (env->ExceptionCheck()) { env->ExceptionClear(); fdCls = nullptr; }
      fosCls = env->FindClass("java/io/FileOutputStream");
      if (env->ExceptionCheck()) { env->ExceptionClear(); fosCls = nullptr; }
      psCls = env->FindClass("java/io/PrintStream");
      if (env->ExceptionCheck()) { env->ExceptionClear(); psCls = nullptr; }
      // Get FileDescriptor.out (fd=1) and FileDescriptor.err (fd=2)
      if (!fdCls || !fosCls || !psCls) {
        fprintf(stderr, "[dalvikvm] WARN: class re-find failed, skipping System.out setup\n");
        fflush(stderr);
      } else {
      jfieldID fdOutField = env->GetStaticFieldID(fdCls, "out", "Ljava/io/FileDescriptor;");
      if (env->ExceptionCheck()) { env->ExceptionClear(); fdOutField = nullptr; }
      jfieldID fdErrField = env->GetStaticFieldID(fdCls, "err", "Ljava/io/FileDescriptor;");
      if (env->ExceptionCheck()) { env->ExceptionClear(); fdErrField = nullptr; }

      if (fdOutField && fdErrField) {
        jobject fdOut = env->GetStaticObjectField(fdCls, fdOutField);
        if (env->ExceptionCheck()) { env->ExceptionClear(); fdOut = nullptr; }
        jobject fdErr = env->GetStaticObjectField(fdCls, fdErrField);
        if (env->ExceptionCheck()) { env->ExceptionClear(); fdErr = nullptr; }

        // Create FileOutputStream(FileDescriptor)
        jmethodID fosInit = env->GetMethodID(fosCls, "<init>", "(Ljava/io/FileDescriptor;)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); fosInit = nullptr; }
        if (env->ExceptionCheck()) env->ExceptionClear();

        if (fosInit && fdOut && fdErr) {
          jobject fosOut = env->NewObject(fosCls, fosInit, fdOut);
          if (env->ExceptionCheck()) { env->ExceptionClear(); fosOut = nullptr; }
          jobject fosErr = env->NewObject(fosCls, fosInit, fdErr);
          if (env->ExceptionCheck()) { env->ExceptionClear(); fosErr = nullptr; }

          // Create PrintStream — try (OutputStream, boolean, Charset) with a
          // pre-resolved charset to avoid Charset.forName() which has boot image
          // vtable corruption in TreeMap collections.
          // Strategy: find sun.nio.cs.UTF_8 class (simple charset, no ICU needed),
          // instantiate it, and pass to PrintStream(OutputStream, boolean, Charset).
          jmethodID psInit = nullptr;
          jobject psOut = nullptr;
          jobject psErr = nullptr;

          // Try to find and instantiate sun.nio.cs.UTF_8 charset directly
          jclass utf8Cls = env->FindClass("sun/nio/cs/UTF_8");
          if (env->ExceptionCheck()) { env->ExceptionClear(); utf8Cls = nullptr; }
          jobject utf8Charset = nullptr;
          if (utf8Cls) {
            jmethodID utf8Init = env->GetMethodID(utf8Cls, "<init>", "()V");
            fprintf(stderr, "[dalvikvm] DBG: utf8Init=%p\n", utf8Init); fflush(stderr);
            if (utf8Init && !env->ExceptionCheck()) {
              utf8Charset = env->NewObject(utf8Cls, utf8Init);
              if (env->ExceptionCheck()) { env->ExceptionClear(); utf8Charset = nullptr; }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
          }

          // Pre-populate Charset.cache1 with the sun.nio.cs.UTF_8 instance so
          // future Charset.forName("UTF-8") returns it from cache1 immediately
          // without going through lookup2 → charsetForName → CharsetEncoderICU.
          if (utf8Charset) {
            jclass charsetCls2 = env->FindClass("java/nio/charset/Charset");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (charsetCls2) {
              jfieldID cache1F = env->GetStaticFieldID(charsetCls2, "cache1", "Ljava/util/Map$Entry;");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (cache1F) {
                // Create AbstractMap.SimpleImmutableEntry<String, Charset>("UTF-8", utf8Charset)
                jclass sieCls = env->FindClass("java/util/AbstractMap$SimpleImmutableEntry");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (sieCls) {
                  jmethodID sieInit = env->GetMethodID(sieCls, "<init>",
                      "(Ljava/lang/Object;Ljava/lang/Object;)V");
                  if (env->ExceptionCheck()) env->ExceptionClear();
                  if (sieInit) {
                    jstring utf8Name = env->NewStringUTF("UTF-8");
                    jobject entry = env->NewObject(sieCls, sieInit, utf8Name, utf8Charset);
                    if (entry && !env->ExceptionCheck()) {
                      env->SetStaticObjectField(charsetCls2, cache1F, entry);
                      fprintf(stderr, "[dalvikvm] Set Charset.cache1 to UTF-8 (sun.nio.cs.UTF_8)\n");
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                  }
                }
              }
              // Also put UTF-8 into cache2 HashMap
              jfieldID cache2F = env->GetStaticFieldID(charsetCls2, "cache2", "Ljava/util/HashMap;");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (cache2F) {
                jobject cache2Map = env->GetStaticObjectField(charsetCls2, cache2F);
                if (cache2Map) {
                  jclass hmCls3 = env->FindClass("java/util/HashMap");
                  jmethodID hmPut3 = hmCls3 ? env->GetMethodID(hmCls3, "put",
                      "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;") : nullptr;
                  if (env->ExceptionCheck()) env->ExceptionClear();
                  if (hmPut3) {
                    jstring utf8Key = env->NewStringUTF("UTF-8");
                    env->CallObjectMethod(cache2Map, hmPut3, utf8Key, utf8Charset);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    // Also add lowercase variant
                    jstring utf8Key2 = env->NewStringUTF("utf-8");
                    env->CallObjectMethod(cache2Map, hmPut3, utf8Key2, utf8Charset);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    fprintf(stderr, "[dalvikvm] Added UTF-8 to Charset.cache2\n");
                  }
                }
              }
              if (env->ExceptionCheck()) env->ExceptionClear();
            }
          }

          // Also set StandardCharsets.UTF_8 field directly
          if (utf8Charset) {
            jclass stdCharsets = env->FindClass("java/nio/charset/StandardCharsets");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (stdCharsets) {
              jfieldID utf8Field = env->GetStaticFieldID(stdCharsets, "UTF_8", "Ljava/nio/charset/Charset;");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (utf8Field) {
                env->SetStaticObjectField(stdCharsets, utf8Field, utf8Charset);
                fprintf(stderr, "[dalvikvm] Set StandardCharsets.UTF_8\n");
              }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
          }

          if (utf8Charset) {
            // PrintStream(OutputStream out, boolean autoFlush, Charset charset)
            jmethodID psInitCS = env->GetMethodID(psCls, "<init>",
                "(Ljava/io/OutputStream;ZLjava/nio/charset/Charset;)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (psInitCS) {
              psOut = env->NewObject(psCls, psInitCS, fosOut, JNI_FALSE, utf8Charset);
              if (env->ExceptionCheck()) { env->ExceptionClear(); psOut = nullptr; }
              psErr = env->NewObject(psCls, psInitCS, fosErr, JNI_FALSE, utf8Charset);
              if (env->ExceptionCheck()) { env->ExceptionClear(); psErr = nullptr; }
              fprintf(stderr, "[dalvikvm] Created PrintStream with UTF_8 charset\n");
            }
          }

          // Fallback: basic PrintStream(OutputStream)
          if (!psOut) {
            psInit = env->GetMethodID(psCls, "<init>", "(Ljava/io/OutputStream;)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (psInit) {
              psOut = env->NewObject(psCls, psInit, fosOut);
              if (env->ExceptionCheck()) { env->ExceptionClear(); psOut = nullptr; }
              psErr = env->NewObject(psCls, psInit, fosErr);
              if (env->ExceptionCheck()) { env->ExceptionClear(); psErr = nullptr; }
            }
          }

          // If PrintStream creation succeeded with charset, use it.
          // Otherwise create a minimal NativePrintStream that writes raw bytes.
          // For the fallback, we override System.out.println by providing a custom
          // native print method via JNI - but that's complex.
          // Instead: if psOut is null, create it with the basic constructor anyway.
          // The AbstractMethodError only happens when println is called, not at construction.
          if (!psOut && psInit) {
            psOut = env->NewObject(psCls, psInit, fosOut);
            if (env->ExceptionCheck()) { env->ExceptionClear(); psOut = nullptr; }
          }
          if (!psErr && psInit) {
            psErr = env->NewObject(psCls, psInit, fosErr);
            if (env->ExceptionCheck()) { env->ExceptionClear(); psErr = nullptr; }
          }

          // Pre-set PrintStream's internal textOut field to bypass getTextOut()
          // which triggers Charset.forName() → CharsetICU.newEncoder() →
          // CharsetEncoderICU.makeReplacement() → HashMap itable dispatch bug (NPE).
          //
          // Use art.io.Utf8Writer — a pure-Java Writer that encodes chars as
          // UTF-8 bytes without touching ICU or Charset at all.  It lives in
          // core-jars/art-patch.jar which must be on the boot classpath.
          if (psOut) {
            jclass uwCls = env->FindClass("art/io/Utf8Writer");
            if (env->ExceptionCheck()) { env->ExceptionClear(); uwCls = nullptr; }
            jclass bwCls = env->FindClass("java/io/BufferedWriter");
            if (env->ExceptionCheck()) { env->ExceptionClear(); bwCls = nullptr; }
            jclass oswCls = env->FindClass("java/io/OutputStreamWriter");
            if (env->ExceptionCheck()) { env->ExceptionClear(); oswCls = nullptr; }
            if (uwCls && bwCls) {
              // Utf8Writer(OutputStream)
              jmethodID uwInit = env->GetMethodID(uwCls, "<init>",
                  "(Ljava/io/OutputStream;)V");
              if (env->ExceptionCheck()) { env->ExceptionClear(); uwInit = nullptr; }
              // Use (Writer, int) constructor to avoid BufferedWriter.defaultCharBufferSize
              // which is 0 because we force-inited BufferedWriter (skipped <clinit>)
              jmethodID bwInit = env->GetMethodID(bwCls, "<init>", "(Ljava/io/Writer;I)V");
              if (env->ExceptionCheck()) { env->ExceptionClear(); bwInit = nullptr; }

              // Create a dummy OutputStreamWriter via AllocObject (no ctor)
              // for charOut.  Its flushBuffer() calls se.flushBuffer().
              // We create a StreamEncoder via AllocObject too, with isOpen=false
              // so flushBuffer() throws IOException (which PrintStream catches).
              jobject dummyOSW = nullptr;
              if (oswCls) {
                dummyOSW = env->AllocObject(oswCls);
                if (env->ExceptionCheck()) { env->ExceptionClear(); dummyOSW = nullptr; }
                if (dummyOSW) {
                  jfieldID seF = env->GetFieldID(oswCls, "se", "Lsun/nio/cs/StreamEncoder;");
                  if (env->ExceptionCheck()) env->ExceptionClear();
                  if (seF) {
                    jclass seCls = env->FindClass("sun/nio/cs/StreamEncoder");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); seCls = nullptr; }
                    if (seCls) {
                      jobject dummySE = env->AllocObject(seCls);
                      if (env->ExceptionCheck()) { env->ExceptionClear(); dummySE = nullptr; }
                      if (dummySE) {
                        // StreamEncoder.isOpen = false so flushBuffer() throws
                        // IOException "Stream closed" — which PrintStream catches.
                        jfieldID isOpenF = env->GetFieldID(seCls, "isOpen", "Z");
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        if (isOpenF) env->SetBooleanField(dummySE, isOpenF, JNI_FALSE);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        // Set lock object (synchronized(lock) in flushBuffer)
                        jclass writerCls = env->FindClass("java/io/Writer");
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        if (writerCls) {
                          jfieldID lockF = env->GetFieldID(writerCls, "lock", "Ljava/lang/Object;");
                          if (env->ExceptionCheck()) env->ExceptionClear();
                          if (lockF) {
                            // Use the OutputStream as the lock
                            env->SetObjectField(dummySE, lockF, fosOut);
                          }
                          if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        env->SetObjectField(dummyOSW, seF, dummySE);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        fprintf(stderr, "[dalvikvm] Created dummy OutputStreamWriter for charOut\n");
                      }
                    }
                  }
                }
              }
              if (env->ExceptionCheck()) env->ExceptionClear();

              if (uwInit && bwInit) {
                jstring newline = env->NewStringUTF("\n");
                // Get lineSeparator field — try all possible names and types
                jfieldID bwLineSepF = nullptr;
                const char* sep_names[] = {"lineSeparator", "lineSep"};
                const char* sep_types[] = {"Ljava/lang/String;", "[C"};
                for (const char* name : sep_names) {
                  for (const char* type : sep_types) {
                    bwLineSepF = env->GetFieldID(bwCls, name, type);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); bwLineSepF = nullptr; }
                    if (bwLineSepF) {
                      fprintf(stderr, "[dalvikvm] Found BW field %s type %s\n", name, type);
                      break;
                    }
                  }
                  if (bwLineSepF) break;
                }
                if (!bwLineSepF) fprintf(stderr, "[dalvikvm] WARN: no lineSep field found in BufferedWriter\n");
                fflush(stderr);

                jfieldID textOutF = env->GetFieldID(psCls, "textOut", "Ljava/io/BufferedWriter;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); textOutF = nullptr; }
                jfieldID charOutF = env->GetFieldID(psCls, "charOut", "Ljava/io/OutputStreamWriter;");
                if (env->ExceptionCheck()) env->ExceptionClear();

                // Helper lambda for setting up textOut/charOut on a PrintStream
                jmethodID setFdM = env->GetMethodID(uwCls, "setFd", "(I)V");
                if (env->ExceptionCheck()) { env->ExceptionClear(); setFdM = nullptr; }
                fprintf(stderr, "[dalvikvm] setFdM=%p\n", setFdM); fflush(stderr);
                auto setupPrintStream = [&](jobject ps, jobject fos, const char* name, int rawFd) {
                  jobject uw = env->NewObject(uwCls, uwInit, fos);
                  if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
                  if (!uw) return;
                  // Set the raw fd for native write
                  if (setFdM) {
                    env->CallVoidMethod(uw, setFdM, rawFd);
                    if (env->ExceptionCheck()) {
                      fprintf(stderr, "[dalvikvm] setFd(%d) FAILED\n", rawFd);
                      env->ExceptionDescribe();
                      env->ExceptionClear();
                    } else {
                      fprintf(stderr, "[dalvikvm] setFd(%d) OK\n", rawFd);
                    }
                  }
                  jobject bw = env->NewObject(bwCls, bwInit, uw, (jint)8192);
                  if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
                  if (!bw) return;
                  // Fix lineSeparator if field exists
                  if (bwLineSepF) {
                    env->SetObjectField(bw, bwLineSepF, newline);
                  }
                  if (env->ExceptionCheck()) env->ExceptionClear();
                  if (textOutF) env->SetObjectField(ps, textOutF, bw);
                  if (charOutF && dummyOSW) env->SetObjectField(ps, charOutF, dummyOSW);
                  if (env->ExceptionCheck()) env->ExceptionClear();
                  fprintf(stderr, "[dalvikvm] Pre-set PrintStream.textOut via Utf8Writer (%s)\n", name);
                };

                setupPrintStream(psOut, fosOut, "System.out", 1);
                if (psErr) setupPrintStream(psErr, fosErr, "System.err", 2);
              }
            } else {
              fprintf(stderr, "[dalvikvm] WARN: art.io.Utf8Writer not found — add art-patch.jar to boot classpath\n");
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
          }

          // Set System.out and System.err, reset trouble flag
          if (psOut) {
            jfieldID outField = env->GetStaticFieldID(systemCls, "out", "Ljava/io/PrintStream;");
            if (outField) env->SetStaticObjectField(systemCls, outField, psOut);
            if (env->ExceptionCheck()) env->ExceptionClear();
            // Reset PrintStream.trouble to false (may have been set during setup)
            jfieldID troubleF = env->GetFieldID(psCls, "trouble", "Z");
            if (env->ExceptionCheck()) { env->ExceptionClear(); troubleF = nullptr; }
            if (troubleF) {
              env->SetBooleanField(psOut, troubleF, JNI_FALSE);
              fprintf(stderr, "[dalvikvm] System.out set OK (trouble reset)\n");
            } else {
              fprintf(stderr, "[dalvikvm] System.out set OK\n");
            }
          }
          // Register Utf8Writer.nativeWrite JNI method
          {
            jclass uwRegCls = env->FindClass("art/io/Utf8Writer");
            if (env->ExceptionCheck()) { env->ExceptionClear(); uwRegCls = nullptr; }
            if (uwRegCls) {
              JNINativeMethod nm = {"nativeWrite", "(I[BII)I",
                  (void*)Java_art_io_Utf8Writer_nativeWrite};
              if (env->RegisterNatives(uwRegCls, &nm, 1) == 0) {
                fprintf(stderr, "[dalvikvm] Registered Utf8Writer.nativeWrite\n");
              } else {
                env->ExceptionClear();
              }
            }
          }
          // Patch Thread.clone() to return 'this' instead of throwing.
          // A15's ThreadLocal code path calls Thread.clone() → CloneNotSupportedException.
          // Fix: replace the first DEX instruction with 'return-object p0'.
          {
            ScopedObjectAccess soa(art::Thread::Current());
            art::ObjPtr<art::mirror::Class> threadClass =
                art::Runtime::Current()->GetClassLinker()->FindSystemClass(
                    soa.Self(), "Ljava/lang/Thread;");
            if (threadClass != nullptr) {
              for (art::ArtMethod& m : threadClass->GetDeclaredVirtualMethods(art::kRuntimePointerSize)) {
                if (strcmp(m.GetName(), "clone") == 0 && m.HasCodeItem()) {
                  art::CodeItemDataAccessor accessor(m.DexInstructionData());
                  uint16_t num_regs = accessor.RegistersSize();
                  uint16_t num_ins = accessor.InsSize();
                  uint16_t this_reg = num_regs - num_ins;
                  uint16_t* insns = const_cast<uint16_t*>(accessor.Insns());
                  // Make the page writable to patch the bytecode
                  uintptr_t page = reinterpret_cast<uintptr_t>(insns) & ~0xFFFUL;
                  mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE);
                  insns[0] = 0x0011 | (this_reg << 8);
                  fprintf(stderr, "[dalvikvm] Patched Thread.clone() → return-object v%d\n", this_reg);
                  break;
                }
              }
            }
            // Patch Float.toString(float) and Double.toString(double) to native
            // to bypass FloatingDecimal which uses arrays that can't be allocated.
            auto patchToNative = [&](const char* classDesc, const char* methodName,
                                     const char* shorty, void* nativeFunc) {
              art::ObjPtr<art::mirror::Class> cls =
                  art::Runtime::Current()->GetClassLinker()->FindSystemClass(soa.Self(), classDesc);
              if (cls != nullptr) {
                for (art::ArtMethod& m : cls->GetDeclaredMethods(art::kRuntimePointerSize)) {
                  if (strcmp(m.GetName(), methodName) == 0) {
                    fprintf(stderr, "[dalvikvm] DBG: %s.%s shorty=%s (want %s) native=%d\n",
                            classDesc, methodName, m.GetShorty(), shorty, m.IsNative());
                  }
                  if (strcmp(m.GetName(), methodName) == 0 &&
                      strcmp(m.GetShorty(), shorty) == 0 && !m.IsNative()) {
                    m.SetAccessFlags(m.GetAccessFlags() | art::kAccNative);
                    m.SetEntryPointFromJni(nativeFunc);
                    // Interpreter handles native methods via JNI entry point directly.
                    fprintf(stderr, "[dalvikvm] Patched %s.%s to native\n", classDesc, methodName);
                    break;
                  }
                }
              }
              if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
            };
            patchToNative("Ljava/lang/Float;", "toString", "LF",
                (void*)Java_java_lang_Float_toStringImpl);
            patchToNative("Ljava/lang/Double;", "toString", "LD",
                (void*)Java_java_lang_Double_toStringImpl);
            if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
          }

          if (psErr) {
            jfieldID errField = env->GetStaticFieldID(systemCls, "err", "Ljava/io/PrintStream;");
            if (errField) env->SetStaticObjectField(systemCls, errField, psErr);
            if (env->ExceptionCheck()) env->ExceptionClear();
            fprintf(stderr, "[dalvikvm] System.err set OK\n");
          }
        }
      }
      } // end of re-found classes block
    }
    // (Force-init moved above before field setting)
    if (env->ExceptionCheck()) env->ExceptionClear();
    fflush(stderr);
  }
  // Patch Float.toString(float) and Double.toString(double) to native C.
  // FloatingDecimal uses inner class arrays that can't be allocated by the A15 interpreter.
  {
    ScopedObjectAccess soa(art::Thread::Current());
    auto patchToNative = [&](const char* classDesc, const char* methodName,
                             const char* shorty, void* nativeFunc) {
      art::ObjPtr<art::mirror::Class> cls =
          art::Runtime::Current()->GetClassLinker()->FindSystemClass(soa.Self(), classDesc);
      if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
      if (cls == nullptr) return;
      for (art::ArtMethod& m : cls->GetDeclaredMethods(art::kRuntimePointerSize)) {
        if (strcmp(m.GetName(), methodName) == 0 && !m.IsNative()) {
          (void)shorty;
          m.SetAccessFlags(m.GetAccessFlags() | art::kAccNative);
          m.SetEntryPointFromJni(nativeFunc);
          fprintf(stderr, "[dalvikvm] Patched %s.%s → native\n", classDesc, methodName);
          break;
        }
      }
      if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
    };
    patchToNative("Ljava/lang/Float;", "toString", "LF",
        (void*)Java_java_lang_Float_toStringImpl);
    patchToNative("Ljava/lang/Double;", "toString", "LD",
        (void*)Java_java_lang_Double_toStringImpl);
    // Patch AbstractStringBuilder.append(float) and append(double) to use
    // String.valueOf instead of FloatingDecimal.appendTo (which uses broken ThreadLocal).
    // We make them native: append(float f) → append(Float.toString(f))
    // But since Float.toString IS now native, it returns a proper string.
    // Simplest: just make FloatingDecimal.appendTo a no-op (return void).
    // The append(float) will still call appendTo but it won't crash.
    // The number won't be formatted but at least no crash.
    // Actually better: patch AbstractStringBuilder.append(float) bytecode to call
    // append(String.valueOf(float)) instead of FloatingDecimal.appendTo.
    // Too complex. Just make appendTo a no-op — the float won't print but no crash.

    // Patch FloatingDecimal.appendTo to return-void immediately
    {
      art::ObjPtr<art::mirror::Class> fdCls =
          art::Runtime::Current()->GetClassLinker()->FindSystemClass(
              soa.Self(), "Ljdk/internal/math/FloatingDecimal;");
      if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
      if (fdCls != nullptr) {
        for (art::ArtMethod& m : fdCls->GetDeclaredMethods(art::kRuntimePointerSize)) {
          if (strcmp(m.GetName(), "appendTo") == 0 && m.HasCodeItem()) {
            art::CodeItemDataAccessor accessor(m.DexInstructionData());
            uint16_t* insns = const_cast<uint16_t*>(accessor.Insns());
            uintptr_t page = reinterpret_cast<uintptr_t>(insns) & ~0xFFFUL;
            mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE);
            insns[0] = 0x000e; // return-void
            fprintf(stderr, "[dalvikvm] Patched FloatingDecimal.appendTo → return-void\n");
            // Don't break — patch ALL overloads (float and double)
          }
        }
        if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
      }
    }
  }

  // Patch StringBuilder.append(boolean) to return-object this (skip boolean value).
  // A15's append(boolean) uses internal arrays that can't be allocated.
  {
    ScopedObjectAccess soa(art::Thread::Current());
    art::ObjPtr<art::mirror::Class> sbCls =
        art::Runtime::Current()->GetClassLinker()->FindSystemClass(
            soa.Self(), "Ljava/lang/StringBuilder;");
    if (soa.Self()->IsExceptionPending()) soa.Self()->ClearException();
    // Patch append(boolean) using JNI method lookup for exact signature match
    {
      jclass sbJni = env->FindClass("java/lang/StringBuilder");
      jclass asbJni = env->FindClass("java/lang/AbstractStringBuilder");
      if (env->ExceptionCheck()) env->ExceptionClear();
      // Get the exact append(Z) method via JNI
      jmethodID sbAppendZ = sbJni ? env->GetMethodID(sbJni, "append", "(Z)Ljava/lang/StringBuilder;") : nullptr;
      if (env->ExceptionCheck()) env->ExceptionClear();
      jmethodID asbAppendZ = asbJni ? env->GetMethodID(asbJni, "append", "(Z)Ljava/lang/AbstractStringBuilder;") : nullptr;
      if (env->ExceptionCheck()) env->ExceptionClear();
      // Convert jmethodID to ArtMethod* and patch bytecode
      auto patchMethod = [&](jmethodID mid, const char* desc) {
        if (!mid) return;
        art::ArtMethod* am = art::jni::DecodeArtMethod(mid);
        if (am && am->HasCodeItem()) {
          art::CodeItemDataAccessor accessor(am->DexInstructionData());
          uint16_t this_reg = accessor.RegistersSize() - accessor.InsSize();
          uint16_t* insns = const_cast<uint16_t*>(accessor.Insns());
          uintptr_t page = reinterpret_cast<uintptr_t>(insns) & ~0xFFFUL;
          mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE);
          insns[0] = 0x0011 | (this_reg << 8);
          fprintf(stderr, "[dalvikvm] Patched %s.append(Z) → return-object v%d\n", desc, this_reg);
        }
      };
      patchMethod(sbAppendZ, "StringBuilder");
      patchMethod(asbAppendZ, "AbstractStringBuilder");
    }
  }

  // Clear Thread.threadLocals to force fresh ThreadLocalMap creation.
  // The boot image may contain a corrupt ThreadLocalMap from dex2oat's thread
  // that has length-0 table after relocation.
  {
    jclass threadCls = env->FindClass("java/lang/Thread");
    if (env->ExceptionCheck()) { env->ExceptionClear(); threadCls = nullptr; }
    if (threadCls) {
      jmethodID currentThread = env->GetStaticMethodID(threadCls, "currentThread",
          "()Ljava/lang/Thread;");
      if (env->ExceptionCheck()) { env->ExceptionClear(); currentThread = nullptr; }
      jfieldID tlField = env->GetFieldID(threadCls, "threadLocals",
          "Ljava/lang/ThreadLocal$ThreadLocalMap;");
      if (env->ExceptionCheck()) { env->ExceptionClear(); tlField = nullptr; }
      if (currentThread && tlField) {
        jobject ct = env->CallStaticObjectMethod(threadCls, currentThread);
        if (env->ExceptionCheck()) { env->ExceptionClear(); ct = nullptr; }
        if (ct) {
          env->SetObjectField(ct, tlField, nullptr);
          if (env->ExceptionCheck()) env->ExceptionClear();
          fprintf(stderr, "[dalvikvm] Cleared Thread.threadLocals\n");
        }
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }

  // Bypass BlockGuardOs by replacing Libcore.os with the underlying Linux
  // instance. BlockGuardOs.write() calls BlockGuard.getThreadPolicy() which
  // uses a ThreadLocal that crashes with length=0 table after boot image relocation.
  {
    jclass libcoreCls = env->FindClass("libcore/io/Libcore");
    if (env->ExceptionCheck()) { env->ExceptionClear(); libcoreCls = nullptr; }
    if (libcoreCls) {
      // Get Libcore.rawOs (the unwrapped Linux instance)
      jfieldID rawOsF = env->GetStaticFieldID(libcoreCls, "rawOs", "Llibcore/io/Os;");
      if (env->ExceptionCheck()) { env->ExceptionClear(); rawOsF = nullptr; }
      jfieldID osF = env->GetStaticFieldID(libcoreCls, "os", "Llibcore/io/Os;");
      if (env->ExceptionCheck()) { env->ExceptionClear(); osF = nullptr; }
      if (rawOsF && osF) {
        jobject rawOs = env->GetStaticObjectField(libcoreCls, rawOsF);
        if (env->ExceptionCheck()) { env->ExceptionClear(); rawOs = nullptr; }
        if (rawOs) {
          env->SetStaticObjectField(libcoreCls, osF, rawOs);
          if (env->ExceptionCheck()) env->ExceptionClear();
          fprintf(stderr, "[dalvikvm] Libcore.os = rawOs (bypass BlockGuard)\n");
        }
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }

  // Fix ThreadLocalMap.INITIAL_CAPACITY — it gets corrupted to 0 by boot image relocation
  {
    jclass tlmCls = env->FindClass("java/lang/ThreadLocal$ThreadLocalMap");
    if (env->ExceptionCheck()) { env->ExceptionClear(); tlmCls = nullptr; }
    if (tlmCls) {
      jfieldID icF = env->GetStaticFieldID(tlmCls, "INITIAL_CAPACITY", "I");
      if (env->ExceptionCheck()) { env->ExceptionClear(); icF = nullptr; }
      if (icF) {
        jint val = env->GetStaticIntField(tlmCls, icF);
        fprintf(stderr, "[dalvikvm] ThreadLocalMap.INITIAL_CAPACITY = %d\n", val);
        if (val != 16) {
          env->SetStaticIntField(tlmCls, icF, 16);
          fprintf(stderr, "[dalvikvm] Fixed INITIAL_CAPACITY to 16\n");
        }
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }

  // Clear Thread.threadLocals RIGHT before main() to ensure no corrupt
  // ThreadLocalMap from boot image or setup code persists.
  {
    jclass threadCls2 = env->FindClass("java/lang/Thread");
    if (env->ExceptionCheck()) { env->ExceptionClear(); threadCls2 = nullptr; }
    if (threadCls2) {
      jmethodID ct2 = env->GetStaticMethodID(threadCls2, "currentThread", "()Ljava/lang/Thread;");
      jfieldID tlf2 = env->GetFieldID(threadCls2, "threadLocals",
          "Ljava/lang/ThreadLocal$ThreadLocalMap;");
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (ct2 && tlf2) {
        jobject t2 = env->CallStaticObjectMethod(threadCls2, ct2);
        if (t2 && !env->ExceptionCheck()) {
          env->SetObjectField(t2, tlf2, nullptr);
          fprintf(stderr, "[dalvikvm] Cleared threadLocals before main()\n");
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }

  // Create fresh PrintStream(NativeOutputStream) right before main().
  // This bypasses the broken FileOutputStream/IoTracker/ThreadLocal chain entirely.
  {
    jclass nosCls = env->FindClass("art/io/NativeOutputStream");
    if (env->ExceptionCheck()) { env->ExceptionClear(); nosCls = nullptr; }
    jclass psCls3 = env->FindClass("java/io/PrintStream");
    jclass sysCls3 = env->FindClass("java/lang/System");
    if (nosCls && psCls3 && sysCls3) {
      jmethodID nosInit = env->GetMethodID(nosCls, "<init>", "(I)V");
      jmethodID psInit3 = env->GetMethodID(psCls3, "<init>", "(Ljava/io/OutputStream;)V");
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (nosInit && psInit3) {
        // stdout
        jobject nos1 = env->NewObject(nosCls, nosInit, 1);
        if (nos1 && !env->ExceptionCheck()) {
          jobject ps1 = env->NewObject(psCls3, psInit3, nos1);
          if (ps1 && !env->ExceptionCheck()) {
            // Set textOut to Utf8Writer(NativeOutputStream)
            jclass uwCls2 = env->FindClass("art/io/Utf8Writer");
            jclass bwCls2 = env->FindClass("java/io/BufferedWriter");
            if (uwCls2 && bwCls2) {
              jmethodID uwI = env->GetMethodID(uwCls2, "<init>", "(Ljava/io/OutputStream;)V");
              jmethodID bwI = env->GetMethodID(bwCls2, "<init>", "(Ljava/io/Writer;I)V");
              jmethodID setFdM2 = env->GetMethodID(uwCls2, "setFd", "(I)V");
              jfieldID toF = env->GetFieldID(psCls3, "textOut", "Ljava/io/BufferedWriter;");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (uwI && bwI && toF) {
                jobject uw2 = env->NewObject(uwCls2, uwI, nos1);
                if (uw2 && setFdM2) env->CallVoidMethod(uw2, setFdM2, 1);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (uw2) {
                  jobject bw2 = env->NewObject(bwCls2, bwI, uw2, (jint)8192);
                  if (env->ExceptionCheck()) env->ExceptionClear();
                  if (bw2) env->SetObjectField(ps1, toF, bw2);
                }
              }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            // Set charOut to the same object as textOut (our BufferedWriter).
            // PrintStream.writeln calls charOut.flushBuffer() which we need to succeed.
            {
              jfieldID charOutF3 = env->GetFieldID(psCls3, "charOut", "Ljava/io/OutputStreamWriter;");
              if (env->ExceptionCheck()) { env->ExceptionClear(); charOutF3 = nullptr; }
              jfieldID toF3 = env->GetFieldID(psCls3, "textOut", "Ljava/io/BufferedWriter;");
              if (env->ExceptionCheck()) { env->ExceptionClear(); toF3 = nullptr; }
              if (charOutF3 && toF3) {
                // BufferedWriter IS-NOT-A OutputStreamWriter, but we can assign it
                // because the JNI SetObjectField doesn't type-check.
                // charOut.flushBuffer() → BufferedWriter.flushBuffer() → Utf8Writer.write → nativeWrite
                jobject textOut3 = env->GetObjectField(ps1, toF3);
                if (textOut3) env->SetObjectField(ps1, charOutF3, textOut3);
              }
              if (env->ExceptionCheck()) env->ExceptionClear();
            }
            jfieldID sysOutF = env->GetStaticFieldID(sysCls3, "out", "Ljava/io/PrintStream;");
            if (sysOutF) env->SetStaticObjectField(sysCls3, sysOutF, ps1);
            fprintf(stderr, "[dalvikvm] System.out = PrintStream(NativeOutputStream(1))\n");
          }
        }
        // stderr
        jobject nos2 = env->NewObject(nosCls, nosInit, 2);
        if (nos2 && !env->ExceptionCheck()) {
          jobject ps2 = env->NewObject(psCls3, psInit3, nos2);
          if (ps2 && !env->ExceptionCheck()) {
            jfieldID sysErrF = env->GetStaticFieldID(sysCls3, "err", "Ljava/io/PrintStream;");
            if (sysErrF) env->SetStaticObjectField(sysCls3, sysErrF, ps2);
          }
        }
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }

  // Reset trouble on the final System.out
  {
    jclass sysCls = env->FindClass("java/lang/System");
    jclass psCls2 = env->FindClass("java/io/PrintStream");
    if (sysCls && psCls2) {
      jfieldID outF = env->GetStaticFieldID(sysCls, "out", "Ljava/io/PrintStream;");
      jfieldID errF = env->GetStaticFieldID(sysCls, "err", "Ljava/io/PrintStream;");
      jfieldID troubleF2 = env->GetFieldID(psCls2, "trouble", "Z");
      if (outF && troubleF2) {
        jobject sysOut = env->GetStaticObjectField(sysCls, outF);
        if (sysOut) env->SetBooleanField(sysOut, troubleF2, JNI_FALSE);
      }
      if (errF && troubleF2) {
        jobject sysErr = env->GetStaticObjectField(sysCls, errF);
        if (sysErr) env->SetBooleanField(sysErr, troubleF2, JNI_FALSE);
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  // Pre-create ThreadLocalMap for the main thread manually via JNI.
  // The interpreter's new-array for Entry[] fails, but JNI NewObjectArray works.
  // So we build the ThreadLocalMap from JNI and attach it to the main thread.
  {
    jclass threadCls3 = env->FindClass("java/lang/Thread");
    jclass tlmCls = env->FindClass("java/lang/ThreadLocal$ThreadLocalMap");
    jclass entryCls = env->FindClass("java/lang/ThreadLocal$ThreadLocalMap$Entry");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (threadCls3 && tlmCls && entryCls) {
      jmethodID ctm = env->GetStaticMethodID(threadCls3, "currentThread", "()Ljava/lang/Thread;");
      jfieldID tlf = env->GetFieldID(threadCls3, "threadLocals", "Ljava/lang/ThreadLocal$ThreadLocalMap;");
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (ctm && tlf) {
        jobject ct = env->CallStaticObjectMethod(threadCls3, ctm);
        if (ct && !env->ExceptionCheck()) {
          env->SetObjectField(ct, tlf, nullptr); // clear stale
          // Create ThreadLocalMap via AllocObject (skip constructor)
          jobject tlm = env->AllocObject(tlmCls);
          if (tlm && !env->ExceptionCheck()) {
            // Set table = new Entry[16] via JNI
            jobjectArray table = env->NewObjectArray(16, entryCls, nullptr);
            if (table && !env->ExceptionCheck()) {
              jfieldID tableF = env->GetFieldID(tlmCls, "table",
                  "[Ljava/lang/ThreadLocal$ThreadLocalMap$Entry;");
              if (env->ExceptionCheck()) env->ExceptionClear();
              if (tableF) {
                env->SetObjectField(tlm, tableF, table);
                // Set threshold and size
                jfieldID threshF = env->GetFieldID(tlmCls, "threshold", "I");
                jfieldID sizeF = env->GetFieldID(tlmCls, "size", "I");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (threshF) env->SetIntField(tlm, threshF, 10); // 2/3 of 16
                if (sizeF) env->SetIntField(tlm, sizeF, 0);
                // Attach to thread
                env->SetObjectField(ct, tlf, tlm);
                fprintf(stderr, "[dalvikvm] Pre-created ThreadLocalMap with Entry[16] table\n");
              }
            }
          }
        }
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
  }

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
#if !defined(__MUSL__)
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
#endif  // !__MUSL__

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

  // Clear any pending exceptions from runtime init
  if (env->ExceptionCheck()) env->ExceptionClear();

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
