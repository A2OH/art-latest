/* Native stubs for framework classes that would normally come from libandroid_runtime.so.
 * These provide minimal implementations sufficient for class initialization.
 */
#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ==================== android.util.Log ==================== */

static jboolean Log_isLoggable(JNIEnv* env, jclass clazz, jstring tag, jint level) {
    return level >= 4; /* INFO and above */
}

static jint Log_println_native(JNIEnv* env, jclass clazz, jint bufID, jint priority,
                                jstring tag, jstring msg) {
    const char* tagStr = tag ? (*env)->GetStringUTFChars(env, tag, NULL) : "?";
    const char* msgStr = msg ? (*env)->GetStringUTFChars(env, msg, NULL) : "";
    fprintf(stderr, "[Log/%d] %s: %s\n", priority, tagStr, msgStr);
    if (tag) (*env)->ReleaseStringUTFChars(env, tag, tagStr);
    if (msg) (*env)->ReleaseStringUTFChars(env, msg, msgStr);
    return 0;
}

static jint Log_logger_entry_max_payload(JNIEnv* env, jclass clazz) {
    return 4068;
}

/* ==================== android.os.SystemProperties ==================== */

static jstring SystemProperties_native_get(JNIEnv* env, jclass clazz, jstring key, jstring def) {
    /* Return hardcoded values for essential properties, otherwise return default */
    if (key != NULL) {
        const char* k = (*env)->GetStringUTFChars(env, key, NULL);
        jstring result = NULL;
        /* Build properties */
        if (strcmp(k, "ro.build.id") == 0) result = (*env)->NewStringUTF(env, "WLK1");
        else if (strcmp(k, "ro.build.display.id") == 0) result = (*env)->NewStringUTF(env, "Westlake-1.0");
        else if (strcmp(k, "ro.product.name") == 0) result = (*env)->NewStringUTF(env, "westlake");
        else if (strcmp(k, "ro.product.device") == 0) result = (*env)->NewStringUTF(env, "ohos");
        else if (strcmp(k, "ro.product.board") == 0) result = (*env)->NewStringUTF(env, "arm64");
        else if (strcmp(k, "ro.product.manufacturer") == 0) result = (*env)->NewStringUTF(env, "Westlake");
        else if (strcmp(k, "ro.product.brand") == 0) result = (*env)->NewStringUTF(env, "westlake");
        else if (strcmp(k, "ro.product.model") == 0) result = (*env)->NewStringUTF(env, "Westlake-OHOS");
        else if (strcmp(k, "ro.hardware") == 0) result = (*env)->NewStringUTF(env, "arm64");
        else if (strcmp(k, "ro.bootloader") == 0) result = (*env)->NewStringUTF(env, "unknown");
        else if (strcmp(k, "ro.build.type") == 0) result = (*env)->NewStringUTF(env, "userdebug");
        else if (strcmp(k, "ro.build.tags") == 0) result = (*env)->NewStringUTF(env, "test-keys");
        else if (strcmp(k, "ro.build.user") == 0) result = (*env)->NewStringUTF(env, "westlake");
        else if (strcmp(k, "ro.build.host") == 0) result = (*env)->NewStringUTF(env, "localhost");
        else if (strcmp(k, "ro.build.fingerprint") == 0) result = (*env)->NewStringUTF(env, "westlake/ohos/ohos:15/WLK/1:userdebug/test-keys");
        else if (strcmp(k, "ro.product.cpu.abilist") == 0) result = (*env)->NewStringUTF(env, "arm64-v8a");
        else if (strcmp(k, "ro.product.cpu.abilist64") == 0) result = (*env)->NewStringUTF(env, "arm64-v8a");
        else if (strcmp(k, "ro.product.cpu.abilist32") == 0) result = (*env)->NewStringUTF(env, "");
        /* Locale properties */
        else if (strcmp(k, "persist.sys.locale") == 0) result = (*env)->NewStringUTF(env, "en-US");
        else if (strcmp(k, "ro.product.locale") == 0) result = (*env)->NewStringUTF(env, "en-US");
        /* Version properties */
        else if (strcmp(k, "ro.build.version.sdk") == 0) result = (*env)->NewStringUTF(env, "35");
        else if (strcmp(k, "ro.build.version.release") == 0) result = (*env)->NewStringUTF(env, "15");
        else if (strcmp(k, "ro.build.version.codename") == 0) result = (*env)->NewStringUTF(env, "REL");
        else if (strcmp(k, "ro.build.version.incremental") == 0) result = (*env)->NewStringUTF(env, "1");
        else if (strcmp(k, "ro.build.version.security_patch") == 0) result = (*env)->NewStringUTF(env, "2025-01-01");
        (*env)->ReleaseStringUTFChars(env, key, k);
        if (result != NULL) return result;
    }
    /* Return the default value, but never null — callers assume non-null */
    if (def == NULL) {
        return (*env)->NewStringUTF(env, "");
    }
    return def;
}

static jstring SystemProperties_native_get_key_only(JNIEnv* env, jclass clazz, jstring key) {
    /* Return empty string, never null — callers assume non-null */
    return (*env)->NewStringUTF(env, "");
}

/* Also return non-null from 2-arg version when def is null */

static void SystemProperties_native_set(JNIEnv* env, jclass clazz, jstring key, jstring val) {
    /* No-op */
}

static jboolean SystemProperties_native_get_boolean(JNIEnv* env, jclass clazz,
                                                     jstring key, jboolean def) {
    return def;
}

static jint SystemProperties_native_get_int(JNIEnv* env, jclass clazz,
                                             jstring key, jint def) {
    return def;
}

static jlong SystemProperties_native_get_long(JNIEnv* env, jclass clazz,
                                               jstring key, jlong def) {
    return def;
}

static void SystemProperties_native_report_sysprop_change(JNIEnv* env, jclass clazz) {
    /* No-op */
}

/* ==================== android.os.Debug ==================== */

static jlong Debug_countInstancesOfClass(JNIEnv* env, jclass clazz, jclass cls, jboolean b) {
    return 0;
}

static jlong Debug_countInstancesOfClasses(JNIEnv* env, jclass clazz, jobjectArray cls, jboolean b) {
    return 0;
}

static jobjectArray Debug_getInstancesOfClasses(JNIEnv* env, jclass clazz, jobjectArray cls, jboolean b) {
    return NULL;
}

static jboolean Debug_isDebuggerConnected(JNIEnv* env, jclass clazz) {
    return JNI_FALSE;
}

/* ==================== android.text.TextUtils ==================== */

/* Replace formatSimple with String.format — the built-in formatSimple only handles %s/%d
 * and throws "Too many arguments" for %x, %08x, etc. */
static jstring TextUtils_formatSimple(JNIEnv* env, jclass clazz, jstring fmt, jobjectArray args) {
    /* Delegate to String.format(String, Object[]) which handles all format specifiers */
    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    if (!stringClass) { (*env)->ExceptionClear(env); return fmt; }
    jmethodID formatMethod = (*env)->GetStaticMethodID(env, stringClass,
        "format", "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;");
    if (!formatMethod) { (*env)->ExceptionClear(env); return fmt; }
    jstring result = (jstring)(*env)->CallStaticObjectMethod(env, stringClass, formatMethod, fmt, args);
    if ((*env)->ExceptionCheck(env)) {
        /* If String.format also fails, just return the format string */
        (*env)->ExceptionClear(env);
        return fmt;
    }
    return result;
}

/* ==================== android.sysprop stubs ==================== */

/* SocProperties.soc_manufacturer() → Optional<String> — returns Optional.of("ARM")
 * SocProperties.soc_model() → Optional<String> — returns Optional.of("Cortex")
 * TelephonyProperties.baseband_version() → List<String> — returns empty list
 * These break circular class init chains that cause StackOverflow during Build.<clinit> */

jobject SocProperties_soc_manufacturer(JNIEnv* env, jclass clazz) {
    jclass optCls = (*env)->FindClass(env, "java/util/Optional");
    if (!optCls) { (*env)->ExceptionClear(env); return NULL; }
    jmethodID of = (*env)->GetStaticMethodID(env, optCls, "of", "(Ljava/lang/Object;)Ljava/util/Optional;");
    if (!of) { (*env)->ExceptionClear(env); return NULL; }
    return (*env)->CallStaticObjectMethod(env, optCls, of, (*env)->NewStringUTF(env, "ARM"));
}

jobject SocProperties_soc_model(JNIEnv* env, jclass clazz) {
    jclass optCls = (*env)->FindClass(env, "java/util/Optional");
    if (!optCls) { (*env)->ExceptionClear(env); return NULL; }
    jmethodID of = (*env)->GetStaticMethodID(env, optCls, "of", "(Ljava/lang/Object;)Ljava/util/Optional;");
    if (!of) { (*env)->ExceptionClear(env); return NULL; }
    return (*env)->CallStaticObjectMethod(env, optCls, of, (*env)->NewStringUTF(env, "Cortex"));
}

jobject TelephonyProperties_baseband_version(JNIEnv* env, jclass clazz) {
    jclass listCls = (*env)->FindClass(env, "java/util/Collections");
    if (!listCls) { (*env)->ExceptionClear(env); return NULL; }
    jmethodID emptyList = (*env)->GetStaticMethodID(env, listCls, "emptyList", "()Ljava/util/List;");
    if (!emptyList) { (*env)->ExceptionClear(env); return NULL; }
    return (*env)->CallStaticObjectMethod(env, listCls, emptyList);
}

/* CaseMapper stub — returns input unchanged (ASCII doesn't need case mapping) */
static jstring CaseMapper_toLowerCase_stub(JNIEnv* env, jclass cls, jobject locale, jstring input, jint len) {
    return input;
}
static jstring CaseMapper_toUpperCase_stub(JNIEnv* env, jclass cls, jobject locale, jstring input, jint len) {
    return input;
}

/* ==================== Registration ==================== */

static int registerWithTolerance(JNIEnv* env, const char* className,
                                  const JNINativeMethod* methods, int count) {
    jclass cls = (*env)->FindClass(env, className);
    if (!cls) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    int ok = 0;
    for (int i = 0; i < count; i++) {
        if ((*env)->RegisterNatives(env, cls, &methods[i], 1) == 0) {
            ok++;
        } else {
            (*env)->ExceptionClear(env);
        }
    }
    (*env)->DeleteLocalRef(env, cls);
    return ok;
}

jint JNI_OnLoad_framework(void* vm, void* reserved) {
    JNIEnv* env;
    if ((*(JavaVM*)vm)->GetEnv((JavaVM*)vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;

    fprintf(stderr, "[fw_stubs] Registering framework native stubs\n");

    /* android.util.Log */
    {
        JNINativeMethod methods[] = {
            {"isLoggable", "(Ljava/lang/String;I)Z", (void*)Log_isLoggable},
            {"println_native", "(IILjava/lang/String;Ljava/lang/String;)I", (void*)Log_println_native},
            {"logger_entry_max_payload_native", "()I", (void*)Log_logger_entry_max_payload},
        };
        int ok = registerWithTolerance(env, "android/util/Log", methods, 3);
        fprintf(stderr, "[fw_stubs] android.util.Log: %d/3\n", ok);
    }

    /* android.os.SystemProperties */
    {
        JNINativeMethod methods[] = {
            {"native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void*)SystemProperties_native_get},
            {"native_get", "(Ljava/lang/String;)Ljava/lang/String;", (void*)SystemProperties_native_get_key_only},
            {"native_set", "(Ljava/lang/String;Ljava/lang/String;)V", (void*)SystemProperties_native_set},
            {"native_get_boolean", "(Ljava/lang/String;Z)Z", (void*)SystemProperties_native_get_boolean},
            {"native_get_int", "(Ljava/lang/String;I)I", (void*)SystemProperties_native_get_int},
            {"native_get_long", "(Ljava/lang/String;J)J", (void*)SystemProperties_native_get_long},
            {"native_report_sysprop_change", "()V", (void*)SystemProperties_native_report_sysprop_change},
        };
        int ok = registerWithTolerance(env, "android/os/SystemProperties", methods, 7);
        fprintf(stderr, "[fw_stubs] android.os.SystemProperties: %d/7\n", ok);
    }

    /* android.sysprop.SocProperties + TelephonyProperties
     * These are Java methods, not native. We can't RegisterNatives on them.
     * Instead, register them tolerantly — if the class/method isn't native,
     * the registration silently fails, and our InterpreterJni resolution
     * (in interpreter.cc) will throw UnsatisfiedLinkError which gets tolerated
     * by clinit tolerance. The Build fields get patched by dalvikvm.cc afterwards.
     *
     * For a proper fix, we need to flip the kAccNative flag first. This is done
     * from C++ in the runtime (dalvikvm.cc late-patch section).
     */
    {
        JNINativeMethod methods[] = {
            {"soc_manufacturer", "()Ljava/util/Optional;", (void*)SocProperties_soc_manufacturer},
            {"soc_model", "()Ljava/util/Optional;", (void*)SocProperties_soc_model},
        };
        int ok = registerWithTolerance(env, "android/sysprop/SocProperties", methods, 2);
        fprintf(stderr, "[fw_stubs] android.sysprop.SocProperties: %d/2\n", ok);
    }
    {
        JNINativeMethod methods[] = {
            {"baseband_version", "()Ljava/util/List;", (void*)TelephonyProperties_baseband_version},
        };
        int ok = registerWithTolerance(env, "android/sysprop/TelephonyProperties", methods, 1);
        fprintf(stderr, "[fw_stubs] android.sysprop.TelephonyProperties: %d/1\n", ok);
    }

    /* Pre-set Locale.ROOT/ENGLISH/US using AllocObject (no constructor call) */
    {
        jclass locCls = (*env)->FindClass(env, "java/util/Locale");
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (locCls) {
            /* AllocObject creates the Java object without calling <init> */
            jobject root = (*env)->AllocObject(env, locCls);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            jobject english = (*env)->AllocObject(env, locCls);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            jobject us = (*env)->AllocObject(env, locCls);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

            /* Set the languageTag field directly (Locale stores a cached tag) */
            /* Locale has: private transient String languageTag;
               private transient BaseLocale baseLocale; */
            /* For ROOT: baseLocale has empty language/country/variant */
            /* We need to also create BaseLocale objects — or just set languageTag */
            /* Actually, many Locale methods check baseLocale first.
               Let's set languageTag which is used by toString()/toLanguageTag() */
            jfieldID tagF = (*env)->GetFieldID(env, locCls, "languageTag", "Ljava/lang/String;");
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

            if (root && tagF) (*env)->SetObjectField(env, root, tagF, (*env)->NewStringUTF(env, "und"));
            if (english && tagF) (*env)->SetObjectField(env, english, tagF, (*env)->NewStringUTF(env, "en"));
            if (us && tagF) (*env)->SetObjectField(env, us, tagF, (*env)->NewStringUTF(env, "en-US"));

            /* Create BaseLocale objects for each Locale */
            jclass blCls = (*env)->FindClass(env, "sun/util/locale/BaseLocale");
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            if (blCls) {
                jstring empty = (*env)->NewStringUTF(env, "");
                jstring en = (*env)->NewStringUTF(env, "en");
                jstring us_s = (*env)->NewStringUTF(env, "US");

                jobject blRoot = (*env)->AllocObject(env, blCls);
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                jobject blEn = (*env)->AllocObject(env, blCls);
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                jobject blUs = (*env)->AllocObject(env, blCls);
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

                /* BaseLocale fields: language, script, region, variant */
                jfieldID langF = (*env)->GetFieldID(env, blCls, "language", "Ljava/lang/String;");
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                jfieldID regF = (*env)->GetFieldID(env, blCls, "region", "Ljava/lang/String;");
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                jfieldID scrF = (*env)->GetFieldID(env, blCls, "script", "Ljava/lang/String;");
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                jfieldID varF = (*env)->GetFieldID(env, blCls, "variant", "Ljava/lang/String;");
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

                if (blRoot && langF) {
                    (*env)->SetObjectField(env, blRoot, langF, empty);
                    if (regF) (*env)->SetObjectField(env, blRoot, regF, empty);
                    if (scrF) (*env)->SetObjectField(env, blRoot, scrF, empty);
                    if (varF) (*env)->SetObjectField(env, blRoot, varF, empty);
                }
                if (blEn && langF) {
                    (*env)->SetObjectField(env, blEn, langF, en);
                    if (regF) (*env)->SetObjectField(env, blEn, regF, empty);
                    if (scrF) (*env)->SetObjectField(env, blEn, scrF, empty);
                    if (varF) (*env)->SetObjectField(env, blEn, varF, empty);
                }
                if (blUs && langF) {
                    (*env)->SetObjectField(env, blUs, langF, en);
                    if (regF) (*env)->SetObjectField(env, blUs, regF, us_s);
                    if (scrF) (*env)->SetObjectField(env, blUs, scrF, empty);
                    if (varF) (*env)->SetObjectField(env, blUs, varF, empty);
                }

                /* Set baseLocale on each Locale */
                jfieldID blF = (*env)->GetFieldID(env, locCls, "baseLocale", "Lsun/util/locale/BaseLocale;");
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                if (blF) {
                    if (root && blRoot) (*env)->SetObjectField(env, root, blF, blRoot);
                    if (english && blEn) (*env)->SetObjectField(env, english, blF, blEn);
                    if (us && blUs) (*env)->SetObjectField(env, us, blF, blUs);
                }
                fprintf(stderr, "[fw_stubs] BaseLocale objects set\n");
            }

            /* Set static fields ROOT, ENGLISH, US */
            jfieldID rf = (*env)->GetStaticFieldID(env, locCls, "ROOT", "Ljava/util/Locale;");
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            if (rf && root) (*env)->SetStaticObjectField(env, locCls, rf, root);

            jfieldID ef = (*env)->GetStaticFieldID(env, locCls, "ENGLISH", "Ljava/util/Locale;");
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            if (ef && english) (*env)->SetStaticObjectField(env, locCls, ef, english);

            jfieldID uf = (*env)->GetStaticFieldID(env, locCls, "US", "Ljava/util/Locale;");
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            if (uf && us) (*env)->SetStaticObjectField(env, locCls, uf, us);

            /* Also set defaultLocale */
            jfieldID df = (*env)->GetStaticFieldID(env, locCls, "defaultLocale", "Ljava/util/Locale;");
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            if (df && us) (*env)->SetStaticObjectField(env, locCls, df, us);

            fprintf(stderr, "[fw_stubs] Locale pre-set via AllocObject: root=%p en=%p us=%p\n", root, english, us);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        }
    }

    /* Patch CaseMapper to return input as-is (avoids ICU dependency) */
    {
        jclass cmCls = (*env)->FindClass(env, "java/lang/CaseMapper");
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (cmCls) {
            /* CaseMapper.toLowerCase(Locale, String, int) returns String */
            /* When ICU is missing, this returns null. Stub it to return input. */
            JNINativeMethod nm = {"toLowerCase",
                "(Ljava/util/Locale;Ljava/lang/String;I)Ljava/lang/String;",
                (void*)CaseMapper_toLowerCase_stub};
            /* CaseMapper methods aren't native — need to flip flag first */
            /* Actually CaseMapper.toLowerCase IS native in Android 15 */
            if ((*env)->RegisterNatives(env, cmCls, &nm, 1) != 0) {
                (*env)->ExceptionClear(env);
            } else {
                fprintf(stderr, "[fw_stubs] CaseMapper.toLowerCase stubbed\n");
            }
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }

    fprintf(stderr, "[fw_stubs] Framework native stubs registered\n");
    return JNI_VERSION_1_6;
}
