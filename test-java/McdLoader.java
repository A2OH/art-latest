import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Constructor;

public class McdLoader {
    static native void nativeLog(String msg);
    static native Object nativeAllocInstance(Class<?> cls);
    static native void nativePrintException(Throwable t);

    static void log(String msg) {
        try { nativeLog(msg); return; } catch (Throwable t) {}
        try {
            java.io.FileOutputStream f = new java.io.FileOutputStream(java.io.FileDescriptor.err);
            byte[] b = new byte[msg.length() + 1];
            for (int i = 0; i < msg.length(); i++) b[i] = (byte)(msg.charAt(i) & 0x7f);
            b[msg.length()] = (byte)'\n';
            f.write(b, 0, b.length);
        } catch (Throwable t2) {}
    }

    public static void main(String[] args) {
        log("=== Westlake ART v118: MCD onCreate ===");

        ClassLoader cl = McdLoader.class.getClassLoader();

        // Locale.ROOT/ENGLISH/US pre-set by JNI_OnLoad_framework (C code via AllocObject)

        // Step 1: Load SplashActivity
        log("[1] Loading SplashActivity...");
        Class<?> splashClass = null;
        try {
            splashClass = Class.forName(
                "com.mcdonalds.mcdcoreapp.common.activity.SplashActivity", false, cl);
            log("[OK] SplashActivity loaded (" + splashClass.getDeclaredMethods().length + " methods)");
        } catch (Throwable t) {
            log("[FAIL] " + t.getClass().getName());
            return;
        }

        // Step 2: Allocate SplashActivity instance (no constructor)
        log("[2] Allocating SplashActivity...");
        Object splash = null;
        try {
            splash = nativeAllocInstance(splashClass);
            log("[OK] Instance via nativeAlloc");
        } catch (Throwable t) {
            // Fallback: JNI AllocObject via env
            log("[WARN] nativeAlloc failed, trying JNI newInstance...");
            try {
                // Use reflection to call JNI AllocObject indirectly
                // Actually just use the default constructor
                Constructor<?> ctor = splashClass.getDeclaredConstructor();
                ctor.setAccessible(true);
                splash = ctor.newInstance();
                log("[OK] Instance via constructor");
            } catch (Throwable t2) {
                log("[WARN] constructor failed: " + t2.getClass().getName());
                // Last resort: allocate parent class
                try {
                    Class<?> actClass = Class.forName("android.app.Activity");
                    Constructor<?> actCtor = actClass.getDeclaredConstructor();
                    actCtor.setAccessible(true);
                    splash = actCtor.newInstance();
                    log("[OK] Activity instance (not SplashActivity)");
                } catch (Throwable t3) {
                    log("[FAIL] All allocation methods failed");
                    return;
                }
            }
        }

        // Step 3: Create mock Context chain
        log("[3] Creating mock Context...");
        Object mockContext = null;
        try {
            // ContextImpl — allocate without constructor
            Class<?> ctxImpl = Class.forName("android.app.ContextImpl", false, cl);
            mockContext = nativeAllocInstance(ctxImpl);
            log("[OK] ContextImpl allocated");

            // Set mPackageName on ContextImpl
            try {
                Field pkgField = ctxImpl.getDeclaredField("mBasePackageName");
                pkgField.setAccessible(true);
                pkgField.set(mockContext, "com.mcdonalds.app");
                log("[OK] mBasePackageName = com.mcdonalds.app");
            } catch (Throwable t) {
                log("[WARN] mPackageName: " + t.getClass().getName());
            }
        } catch (Throwable t) {
            log("[FAIL] Context: " + t.getClass().getName());
        }

        // Step 4: Create mock Instrumentation + Application
        log("[4] Creating Instrumentation + Application...");
        Object instr = null, app = null;
        try {
            instr = nativeAllocInstance(Class.forName("android.app.Instrumentation", false, cl));
            log("[OK] Instrumentation allocated");
        } catch (Throwable t) { log("[WARN] Instrumentation: " + t.getClass().getName()); }
        try {
            app = nativeAllocInstance(Class.forName("android.app.Application", false, cl));
            log("[OK] Application allocated");
        } catch (Throwable t) { log("[WARN] Application: " + t.getClass().getName()); }

        // Step 5: Set Activity fields directly (bypass attach())
        log("[5] Setting Activity fields...");
        try {
            // Activity inherits from ContextThemeWrapper → ContextWrapper → Context
            // ContextWrapper has mBase field
            Class<?> cwClass = Class.forName("android.content.ContextWrapper");
            Field mBase = cwClass.getDeclaredField("mBase");
            mBase.setAccessible(true);
            mBase.set(splash, mockContext);
            log("[OK] mBase = ContextImpl");

            // Activity fields
            Class<?> actClass = Class.forName("android.app.Activity");

            // mInstrumentation
            try {
                Field f = actClass.getDeclaredField("mInstrumentation");
                f.setAccessible(true); f.set(splash, instr);
                log("[OK] mInstrumentation set");
            } catch (Throwable t) { log("[WARN] mInstrumentation: " + t.getClass().getName()); }

            // mApplication
            try {
                Field f = actClass.getDeclaredField("mApplication");
                f.setAccessible(true); f.set(splash, app);
                log("[OK] mApplication set");
            } catch (Throwable t) { log("[WARN] mApplication: " + t.getClass().getName()); }

            // mCalled (must be false before onCreate)
            try {
                Field f = actClass.getDeclaredField("mCalled");
                f.setAccessible(true); f.set(splash, false);
            } catch (Throwable t) {}

            // mComponent
            try {
                Class<?> cnClass = Class.forName("android.content.ComponentName");
                Constructor<?> cnCtor = cnClass.getConstructor(String.class, String.class);
                Object cn = cnCtor.newInstance("com.mcdonalds.app",
                    "com.mcdonalds.mcdcoreapp.common.activity.SplashActivity");
                Field f = actClass.getDeclaredField("mComponent");
                f.setAccessible(true); f.set(splash, cn);
                log("[OK] mComponent set");
            } catch (Throwable t) { log("[WARN] mComponent: " + t.getClass().getName()); }

            // mIntent — SplashActivity.onCreate calls getIntent().getExtras()
            try {
                Class<?> intentClass = Class.forName("android.content.Intent");
                Object intent = nativeAllocInstance(intentClass);
                Field f = actClass.getDeclaredField("mIntent");
                f.setAccessible(true); f.set(splash, intent);
                log("[OK] mIntent set");
            } catch (Throwable t) { log("[WARN] mIntent: " + t.getClass().getName()); }

            // mWindow — many Activity methods need getWindow()
            try {
                Class<?> windowClass = Class.forName("com.android.internal.policy.PhoneWindow");
                Object window = nativeAllocInstance(windowClass);
                Field f = actClass.getDeclaredField("mWindow");
                f.setAccessible(true); f.set(splash, window);
                log("[OK] mWindow set");
            } catch (Throwable t) { log("[WARN] mWindow: " + t.getClass().getName()); }

            // mToken — needed for window operations
            try {
                Class<?> binderClass = Class.forName("android.os.Binder");
                Object token = nativeAllocInstance(binderClass);
                Field f = actClass.getDeclaredField("mToken");
                f.setAccessible(true); f.set(splash, token);
                log("[OK] mToken set");
            } catch (Throwable t) { log("[WARN] mToken: " + t.getClass().getName()); }

        } catch (Throwable t) {
            log("[FAIL] fields: " + t.getClass().getName());
        }

        // Step 6: CALL onCreate(null) !!!
        log("[6] Calling SplashActivity.onCreate(null)...");
        log("    (all bytecode runs through Westlake interpreter)");
        try {
            Method onCreate = splashClass.getDeclaredMethod("onCreate", Class.forName("android.os.Bundle"));
            onCreate.setAccessible(true);
            onCreate.invoke(splash, (Object) null);
            log("[OK] onCreate returned!");
        } catch (Throwable t) {
            // Use native exception printer to avoid StackOverflow from
            // getMessage() / string concat in the interpreter
            try { nativePrintException(t); } catch (Throwable t2) {
                log("[EXEC] nativePrintException failed");
            }
        }

        log("=== Westlake: onCreate execution complete ===");
    }
}
