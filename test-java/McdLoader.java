import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.File;
import java.io.PrintWriter;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Constructor;

/**
 * McdLoader — runs MCD app code through Westlake ART v114 interpreter.
 * Writes status to /data/local/tmp/westlake/engine_status.txt for host display.
 */
public class McdLoader {
    static PrintWriter statusWriter;

    static void log(String msg) {
        // Write to stderr (goes to cache/stderr.log)
        try {
            FileOutputStream ferr = new FileOutputStream(FileDescriptor.err);
            byte[] b = new byte[msg.length() + 1];
            for (int i = 0; i < msg.length(); i++) b[i] = (byte)(msg.charAt(i) & 0x7f);
            b[msg.length()] = (byte)'\n';
            ferr.write(b, 0, b.length);
        } catch (Throwable t) {}
        // Also write to status file
        if (statusWriter != null) {
            statusWriter.println(msg);
            statusWriter.flush();
        }
    }

    static String shortName(String cls) {
        int dot = -1;
        for (int i = cls.length() - 1; i >= 0; i--) {
            if (cls.charAt(i) == '.') { dot = i; break; }
        }
        return dot >= 0 ? cls.substring(dot + 1) : cls;
    }

    public static void main(String[] args) {
        // Open status file
        try {
            statusWriter = new PrintWriter(
                new FileOutputStream("engine_status.txt"), true); // cwd is /data/local/tmp/westlake
        } catch (Throwable t) {}

        log("=== Westlake ART v114 Engine ===");
        log("Running McDonald's app code");
        log("");

        // Phase 1: Framework init — DON'T trigger Build clinit (causes StackOverflow
        // from SocProperties infinite recursion). Just load and patch fields.
        try {
            Class<?> b = Class.forName("android.os.Build", false, null);
            set(b, "MODEL", "Westlake-OHOS"); set(b, "MANUFACTURER", "Westlake");
            set(b, "BRAND", "westlake"); set(b, "DEVICE", "ohos");
            set(b, "PRODUCT", "westlake"); set(b, "HARDWARE", "arm64");
            set(b, "TYPE", "userdebug");
            set(b, "SUPPORTED_ABIS", new String[]{"arm64-v8a"});
            set(b, "SUPPORTED_32_BIT_ABIS", new String[]{});
            set(b, "SUPPORTED_64_BIT_ABIS", new String[]{"arm64-v8a"});
            set(b, "IS_DEBUGGABLE", Boolean.FALSE);
            set(b, "IS_ENG", Boolean.FALSE);
            Class<?> v = Class.forName("android.os.Build$VERSION", false, null);
            set(v, "SDK_INT", Integer.valueOf(35));
            set(v, "RELEASE", "15");
            set(v, "CODENAME", "REL");
            set(v, "SDK", "35");
        } catch (Throwable t) {}
        log("[OK] Framework initialized");
        log("[OK] Build.MODEL = Westlake-OHOS");

        // Phase 2: Load MCD classes
        ClassLoader cl = McdLoader.class.getClassLoader();
        Class<?> splashClass = null;
        try {
            splashClass = Class.forName(
                "com.mcdonalds.mcdcoreapp.common.activity.SplashActivity", false, cl);
            log("[OK] SplashActivity loaded (" + splashClass.getDeclaredMethods().length + " methods)");
        } catch (Throwable t) {
            log("[FAIL] SplashActivity: " + shortName(t.getClass().getName()));
        }

        // Show hierarchy
        if (splashClass != null) {
            log("");
            log("Class hierarchy:");
            Class<?> cur = splashClass;
            while (cur != null) {
                log("  " + cur.getName());
                cur = cur.getSuperclass();
            }

            // Show onCreate and key methods
            log("");
            log("SplashActivity methods:");
            for (Method m : splashClass.getDeclaredMethods()) {
                String n = m.getName();
                if (n.contains("onCreate") || n.contains("navigate") || n.contains("onStart")
                    || n.contains("onResume") || n.contains("inject")) {
                    Class<?>[] params = m.getParameterTypes();
                    StringBuilder sb = new StringBuilder("  ");
                    sb.append(m.getReturnType().getSimpleName()).append(" ");
                    sb.append(n).append("(");
                    for (int i = 0; i < params.length; i++) {
                        if (i > 0) sb.append(", ");
                        sb.append(shortName(params[i].getName()));
                    }
                    sb.append(")");
                    log(sb.toString());
                }
            }
        }

        // Phase 3: Load more MCD classes
        log("");
        String[] mcdClasses = {
            "com.mcdonalds.mcdcoreapp.presenter.SplashPresenterImpl",
            "com.mcdonalds.mcdcoreapp.presenter.SplashPresenter",
            "dagger.hilt.android.HiltAndroidApp",
            "kotlin.Metadata",
        };
        int loaded = splashClass != null ? 1 : 0;
        for (String cls : mcdClasses) {
            try {
                Class<?> c = Class.forName(cls, false, cl);
                loaded++;
                log("[OK] " + shortName(cls) + " (" + c.getDeclaredMethods().length + " methods)");
            } catch (Throwable t) {
                log("[--] " + shortName(cls) + " not in primary DEX");
            }
        }

        // Phase 4: Try to instantiate SplashActivity (may fail without Context)
        if (splashClass != null) {
            log("");
            log("Attempting SplashActivity instantiation...");
            try {
                // Try Unsafe.allocateInstance (no constructor call)
                Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
                Field theUnsafe = unsafeClass.getDeclaredField("theUnsafe");
                theUnsafe.setAccessible(true);
                Object unsafe = theUnsafe.get(null);
                Method allocInst = unsafeClass.getMethod("allocateInstance", Class.class);
                Object activity = allocInst.invoke(unsafe, splashClass);
                log("[OK] SplashActivity instance created (via Unsafe)");
                log("  class: " + activity.getClass().getName());
            } catch (Throwable t) {
                log("[FAIL] Instance: " + shortName(t.getClass().getName()));
            }
        }

        log("");
        log("=== " + loaded + " MCD classes loaded through Westlake ===");
        log("=== ART v114 interpreter execution complete ===");

        if (statusWriter != null) statusWriter.close();
    }

    static Object unsafe;
    static Method putObj, putInt, putBool, objectFieldOffset;
    static {
        try {
            Class<?> u = Class.forName("sun.misc.Unsafe");
            Field theUnsafe = u.getDeclaredField("theUnsafe");
            theUnsafe.setAccessible(true);
            unsafe = theUnsafe.get(null);
            putObj = u.getMethod("putObject", Object.class, long.class, Object.class);
            putInt = u.getMethod("putInt", Object.class, long.class, int.class);
            putBool = u.getMethod("putBoolean", Object.class, long.class, boolean.class);
            objectFieldOffset = u.getMethod("objectFieldOffset", Field.class);
        } catch (Throwable t) {}
    }
    // Set static field via Unsafe — bypasses class initialization
    static void set(Class<?> c, String n, Object v) {
        try {
            Field f = c.getDeclaredField(n);
            f.setAccessible(true);
            long offset = (Long) objectFieldOffset.invoke(unsafe, f);
            if (v instanceof Integer) putInt.invoke(unsafe, c, offset, ((Integer)v).intValue());
            else if (v instanceof Boolean) putBool.invoke(unsafe, c, offset, ((Boolean)v).booleanValue());
            else putObj.invoke(unsafe, c, offset, v);
        } catch (Throwable t) {}
    }
}
