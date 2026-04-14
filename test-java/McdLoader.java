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

            // Set mBasePackageName on ContextImpl
            try {
                Field pkgField = ctxImpl.getDeclaredField("mBasePackageName");
                pkgField.setAccessible(true);
                pkgField.set(mockContext, "com.mcdonalds.app");
                log("[OK] mBasePackageName = com.mcdonalds.app");
            } catch (Throwable t) {
                log("[WARN] mBasePackageName: " + t.getClass().getName());
            }

            // Set mContextType = CONTEXT_TYPE_ACTIVITY (not system context!)
            try {
                // CONTEXT_TYPE_ACTIVITY is typically 1
                Field ctxType = ctxImpl.getDeclaredField("mContextType");
                ctxType.setAccessible(true);
                // Read the static CONTEXT_TYPE_ACTIVITY value
                Field ctaField = ctxImpl.getDeclaredField("CONTEXT_TYPE_ACTIVITY");
                ctaField.setAccessible(true);
                int activityType = ctaField.getInt(null);
                ctxType.set(mockContext, activityType);
                log("[OK] mContextType = CONTEXT_TYPE_ACTIVITY (" + activityType + ")");
            } catch (Throwable t) {
                log("[WARN] mContextType: " + t.getClass().getName());
            }

            // Set mPackageInfo (LoadedApk) — needed for resource access
            try {
                Class<?> loadedApkClass = Class.forName("android.app.LoadedApk", false, cl);
                Object loadedApk = nativeAllocInstance(loadedApkClass);
                // Set minimal fields on LoadedApk
                try {
                    Field pkgF = loadedApkClass.getDeclaredField("mPackageName");
                    pkgF.setAccessible(true);
                    pkgF.set(loadedApk, "com.mcdonalds.app");
                } catch (Throwable t2) {}
                // Set ApplicationInfo — each field independently wrapped
                {
                    Class<?> aiClass = Class.forName("android.content.pm.ApplicationInfo");
                    Object appInfo = nativeAllocInstance(aiClass);
                    // Set individual fields (never let one failure block others)
                    try { Field f = aiClass.getDeclaredField("targetSdkVersion"); f.setAccessible(true); f.setInt(appInfo, 35); } catch (Throwable x) {}
                    try { Field f = aiClass.getDeclaredField("sourceDir"); f.setAccessible(true); f.set(appInfo, "/data/local/tmp/westlake/mcd.apk"); } catch (Throwable x) {}
                    try { Field f = aiClass.getDeclaredField("publicSourceDir"); f.setAccessible(true); f.set(appInfo, "/data/local/tmp/westlake/mcd.apk"); } catch (Throwable x) {}
                    try { Field f = aiClass.getDeclaredField("nativeLibraryDir"); f.setAccessible(true); f.set(appInfo, "/data/local/tmp/westlake"); } catch (Throwable x) {}
                    try { Field f = aiClass.getDeclaredField("processName"); f.setAccessible(true); f.set(appInfo, "com.mcdonalds.app"); } catch (Throwable x) {}
                    try { Field f = aiClass.getDeclaredField("flags"); f.setAccessible(true); f.setInt(appInfo, 0x80084); } catch (Throwable x) {}
                    // packageName in parent class hierarchy
                    for (Class<?> c = aiClass; c != null; c = c.getSuperclass()) {
                        try { Field f = c.getDeclaredField("packageName"); f.setAccessible(true); f.set(appInfo, "com.mcdonalds.app"); break; } catch (Throwable x) {}
                    }
                    for (Class<?> c = aiClass; c != null; c = c.getSuperclass()) {
                        try { Field f = c.getDeclaredField("name"); f.setAccessible(true); f.set(appInfo, "com.mcdonalds.app"); break; } catch (Throwable x) {}
                    }
                    // CRITICAL: set mApplicationInfo on LoadedApk (must not be skipped!)
                    try {
                        Field aiF = loadedApkClass.getDeclaredField("mApplicationInfo");
                        aiF.setAccessible(true);
                        aiF.set(loadedApk, appInfo);
                        log("[OK] ApplicationInfo → LoadedApk");
                    } catch (Throwable x) {
                        log("[FAIL] mApplicationInfo: " + x.getClass().getName());
                    }
                }
                // Set mResDir on LoadedApk
                try {
                    Field rdF = loadedApkClass.getDeclaredField("mResDir");
                    rdF.setAccessible(true);
                    rdF.set(loadedApk, "/data/local/tmp/westlake/mcd.apk");
                } catch (Throwable t2) {}
                // Set mDataDir
                try {
                    Field ddF = loadedApkClass.getDeclaredField("mDataDir");
                    ddF.setAccessible(true);
                    ddF.set(loadedApk, "/data/local/tmp/westlake");
                } catch (Throwable t2) {}
                // Set mActivityThread on LoadedApk (needed for updateApplicationInfo)
                try {
                    Class<?> atClass = Class.forName("android.app.ActivityThread");
                    Object actThread = nativeAllocInstance(atClass);
                    Field atF = loadedApkClass.getDeclaredField("mActivityThread");
                    atF.setAccessible(true);
                    atF.set(loadedApk, actThread);
                    // Also set currentActivityThread() singleton
                    try {
                        Field satF = atClass.getDeclaredField("sCurrentActivityThread");
                        satF.setAccessible(true);
                        satF.set(null, actThread);
                    } catch (Throwable t3) {}
                    // Set mMainThread on Activity too
                    try {
                        Class<?> actCls = Class.forName("android.app.Activity");
                        Field mtF = actCls.getDeclaredField("mMainThread");
                        mtF.setAccessible(true);
                        mtF.set(splash, actThread);
                    } catch (Throwable t3) {}
                    log("[OK] ActivityThread set");
                } catch (Throwable t2) {
                    log("[WARN] ActivityThread: " + t2.getClass().getName());
                }
                // Set mMainThread on ContextImpl (needs ActivityThread too)
                try {
                    Field cmtF = ctxImpl.getDeclaredField("mMainThread");
                    cmtF.setAccessible(true);
                    // Reuse the ActivityThread from LoadedApk
                    Field latF = loadedApkClass.getDeclaredField("mActivityThread");
                    latF.setAccessible(true);
                    Object at = latF.get(loadedApk);
                    cmtF.set(mockContext, at);
                } catch (Throwable t2) {}

                Field piF = ctxImpl.getDeclaredField("mPackageInfo");
                piF.setAccessible(true);
                piF.set(mockContext, loadedApk);
                log("[OK] mPackageInfo = LoadedApk");
            } catch (Throwable t) {
                log("[WARN] mPackageInfo: " + t.getClass().getName());
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
            // Set mActivityLifecycleCallbacks (needed for dispatchActivityPreCreated)
            try {
                Class<?> appCls = Class.forName("android.app.Application");
                Field alcF = appCls.getDeclaredField("mActivityLifecycleCallbacks");
                alcF.setAccessible(true);
                alcF.set(app, new java.util.ArrayList<>());
                // Also set mComponentCallbacks
                try {
                    Field ccF = appCls.getDeclaredField("mComponentCallbacks");
                    ccF.setAccessible(true);
                    ccF.set(app, new java.util.ArrayList<>());
                } catch (Throwable t2) {}
                // Set mBase on Application (it's a ContextWrapper)
                try {
                    Class<?> cwCls = Class.forName("android.content.ContextWrapper");
                    Field mbF = cwCls.getDeclaredField("mBase");
                    mbF.setAccessible(true);
                    mbF.set(app, mockContext);
                } catch (Throwable t2) {}
            } catch (Throwable t2) {}
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

            // mActivityLifecycleCallbacks (needed for dispatchActivityPreCreated)
            try {
                Field alcF = actClass.getDeclaredField("mActivityLifecycleCallbacks");
                alcF.setAccessible(true);
                alcF.set(splash, new java.util.concurrent.CopyOnWriteArrayList<>());
            } catch (Throwable t) {
                // Might be ArrayList instead of CopyOnWriteArrayList
                try {
                    Field alcF = actClass.getDeclaredField("mActivityLifecycleCallbacks");
                    alcF.setAccessible(true);
                    alcF.set(splash, new java.util.ArrayList<>());
                } catch (Throwable t2) {}
            }

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
                // Set Window.mCallback = the Activity (Activity implements Window.Callback)
                try {
                    Class<?> winCls = Class.forName("android.view.Window");
                    Field cbF = winCls.getDeclaredField("mCallback");
                    cbF.setAccessible(true);
                    cbF.set(window, splash); // Activity implements Window.Callback
                    // Also set mWindowControllerCallback
                    Field wcbF = winCls.getDeclaredField("mWindowControllerCallback");
                    wcbF.setAccessible(true);
                    wcbF.set(window, splash); // Activity also implements this
                } catch (Throwable t2) {}
                // Set mContext on Window
                try {
                    Class<?> winCls = Class.forName("android.view.Window");
                    Field ctxF = winCls.getDeclaredField("mContext");
                    ctxF.setAccessible(true);
                    ctxF.set(window, splash); // Activity IS the context
                } catch (Throwable t2) {}
                Field f = actClass.getDeclaredField("mWindow");
                f.setAccessible(true); f.set(splash, window);
                log("[OK] mWindow set (with callbacks)");
            } catch (Throwable t) { log("[WARN] mWindow: " + t.getClass().getName()); }

            // mActivityInfo — Activity needs this for theming
            try {
                Class<?> aiInfoClass = Class.forName("android.content.pm.ActivityInfo");
                Object activityInfo = nativeAllocInstance(aiInfoClass);
                // Set applicationInfo on ActivityInfo
                try {
                    // Get our existing ApplicationInfo from LoadedApk
                    Class<?> ctxImplCls = Class.forName("android.app.ContextImpl");
                    Field piF = ctxImplCls.getDeclaredField("mPackageInfo");
                    piF.setAccessible(true);
                    Object loadedApk = piF.get(mockContext);
                    if (loadedApk != null) {
                        Field aiF = loadedApk.getClass().getDeclaredField("mApplicationInfo");
                        aiF.setAccessible(true);
                        Object appInfo2 = aiF.get(loadedApk);
                        if (appInfo2 != null) {
                            Field appInfoF = aiInfoClass.getField("applicationInfo");
                            appInfoF.setAccessible(true);
                            appInfoF.set(activityInfo, appInfo2);
                        }
                    }
                } catch (Throwable t2) {}
                Field aif = actClass.getDeclaredField("mActivityInfo");
                aif.setAccessible(true);
                aif.set(splash, activityInfo);
                log("[OK] mActivityInfo set");
            } catch (Throwable t) { log("[WARN] mActivityInfo: " + t.getClass().getName()); }

            // mTheme — pre-set to avoid getTheme() reading ApplicationInfo
            try {
                Class<?> ctwClass = Class.forName("android.view.ContextThemeWrapper");
                // Create a Resources.Theme via Resources
                // First, try to set mResources + mTheme
                // Simplest: set the theme resource ID so it uses a default
                try {
                    Field trF = ctwClass.getDeclaredField("mThemeResource");
                    trF.setAccessible(true);
                    trF.setInt(splash, 0x01030005); // android.R.style.Theme
                } catch (Throwable t2) {}
                // Set mResources — create Resources + ResourcesImpl with Configuration
                try {
                    Class<?> resClass = Class.forName("android.content.res.Resources");
                    Object resources = nativeAllocInstance(resClass);
                    // Create ResourcesImpl with a Configuration
                    Class<?> resImplClass = Class.forName("android.content.res.ResourcesImpl");
                    Object resImpl = nativeAllocInstance(resImplClass);
                    // Set Configuration on ResourcesImpl
                    try {
                        Class<?> configClass = Class.forName("android.content.res.Configuration");
                        Object config = nativeAllocInstance(configClass);
                        Field cfF = resImplClass.getDeclaredField("mConfiguration");
                        cfF.setAccessible(true);
                        cfF.set(resImpl, config);
                    } catch (Throwable t3) {}
                    // Set DisplayMetrics on ResourcesImpl
                    try {
                        Class<?> dmClass = Class.forName("android.util.DisplayMetrics");
                        Object dm = nativeAllocInstance(dmClass);
                        // Set reasonable defaults
                        try { dmClass.getDeclaredField("density").setFloat(dm, 2.0f); } catch (Throwable x) {}
                        try { dmClass.getDeclaredField("densityDpi").setInt(dm, 320); } catch (Throwable x) {}
                        try { dmClass.getDeclaredField("widthPixels").setInt(dm, 1080); } catch (Throwable x) {}
                        try { dmClass.getDeclaredField("heightPixels").setInt(dm, 2340); } catch (Throwable x) {}
                        try { dmClass.getDeclaredField("xdpi").setFloat(dm, 420f); } catch (Throwable x) {}
                        try { dmClass.getDeclaredField("ydpi").setFloat(dm, 420f); } catch (Throwable x) {}
                        try { dmClass.getDeclaredField("scaledDensity").setFloat(dm, 2.0f); } catch (Throwable x) {}
                        Field dmF = resImplClass.getDeclaredField("mDisplayMetrics");
                        dmF.setAccessible(true);
                        dmF.set(resImpl, dm);
                    } catch (Throwable t3) {}
                    // Set ResourcesImpl on Resources
                    try {
                        Field riF = resClass.getDeclaredField("mResourcesImpl");
                        riF.setAccessible(true);
                        riF.set(resources, resImpl);
                        log("[OK] Resources + ResourcesImpl + Configuration");
                    } catch (Throwable t3) {
                        log("[WARN] mResourcesImpl: " + t3.getClass().getSimpleName());
                    }
                    if (resources != null) {
                        Field resF = ctwClass.getDeclaredField("mResources");
                        resF.setAccessible(true);
                        resF.set(splash, resources);
                        log("[OK] mResources set");
                        // Also set on ContextImpl
                        try {
                            Class<?> ciClass = Class.forName("android.app.ContextImpl");
                            Field ciResF = ciClass.getDeclaredField("mResources");
                            ciResF.setAccessible(true);
                            ciResF.set(mockContext, resources);
                        } catch (Throwable t3) {}
                        // Create a Theme object directly
                        try {
                            Class<?> themeClass = Class.forName("android.content.res.Resources$Theme");
                            Object theme = nativeAllocInstance(themeClass);
                            // Set mLock (used for synchronization in resolveAttribute)
                            try {
                                Field lockF = themeClass.getDeclaredField("mLock");
                                lockF.setAccessible(true);
                                lockF.set(theme, new Object());
                            } catch (Throwable t4) {
                                // Try 'mKey' or any Object field used as lock
                                for (Field f : themeClass.getDeclaredFields()) {
                                    if (f.getType() == Object.class) {
                                        f.setAccessible(true);
                                        if (f.get(theme) == null) {
                                            f.set(theme, new Object());
                                        }
                                    }
                                }
                            }
                            // Link Theme to Resources
                            try {
                                Field ownerF = themeClass.getDeclaredField("mResources");
                                ownerF.setAccessible(true);
                                ownerF.set(theme, resources);
                            } catch (Throwable t4) {}
                            // Set mTheme on Activity
                            Field mThemeF = ctwClass.getDeclaredField("mTheme");
                            mThemeF.setAccessible(true);
                            mThemeF.set(splash, theme);
                            log("[OK] mTheme pre-set (with mLock)");
                        } catch (Throwable t3) {
                            log("[WARN] mTheme: " + t3.getClass().getName());
                        }
                    }
                } catch (Throwable t2) { log("[WARN] mResources: " + t2.getClass().getName()); }
            } catch (Throwable t) { log("[WARN] theme: " + t.getClass().getName()); }

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

        // Step 7: Try calling AppCompatActivity.onCreate directly if SplashActivity failed
        log("[7] Trying AppCompatActivity.onCreate...");
        try {
            Class<?> appCompatCls = Class.forName("androidx.appcompat.app.AppCompatActivity");
            Method acOnCreate = appCompatCls.getDeclaredMethod("onCreate", Class.forName("android.os.Bundle"));
            acOnCreate.setAccessible(true);
            acOnCreate.invoke(splash, (Object) null);
            log("[OK] AppCompatActivity.onCreate returned!");
        } catch (Throwable t) {
            try { nativePrintException(t); } catch (Throwable t2) {}
        }

        // Step 8: Try calling Activity.performCreate directly
        log("[8] Trying Activity.performCreate...");
        try {
            Class<?> actCls = Class.forName("android.app.Activity");
            // performCreate(Bundle, PersistableBundle)
            Method perfCreate = null;
            for (Method m : actCls.getDeclaredMethods()) {
                if (m.getName().equals("performCreate") && m.getParameterCount() == 1) {
                    perfCreate = m;
                    break;
                }
            }
            if (perfCreate != null) {
                perfCreate.setAccessible(true);
                perfCreate.invoke(splash, (Object) null);
                log("[OK] Activity.performCreate returned!");
            }
        } catch (Throwable t) {
            try { nativePrintException(t); } catch (Throwable t2) {}
        }

        log("=== Westlake: onCreate execution complete ===");
    }
}
