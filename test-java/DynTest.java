import java.io.FileDescriptor;
import java.io.FileOutputStream;
public class DynTest {
    static void log(String msg) {
        try {
            FileOutputStream ferr = new FileOutputStream(FileDescriptor.err);
            byte[] b = new byte[msg.length() + 1];
            for (int i = 0; i < msg.length(); i++) b[i] = (byte)(msg.charAt(i) & 0x7f);
            b[msg.length()] = (byte)'\n';
            ferr.write(b, 0, b.length);
        } catch (Throwable t) {}
    }
    static String shortName(String cls) {
        // Avoid lastIndexOf(int) — use indexOf loop instead
        int dot = -1;
        for (int i = cls.length() - 1; i >= 0; i--) {
            if (cls.charAt(i) == '.') { dot = i; break; }
        }
        return dot >= 0 ? cls.substring(dot + 1) : cls;
    }
    public static void main(String[] args) {
        log("[WL] === Westlake MCD Load ===");
        try { Class.forName("android.os.Build", true, null); } catch (Throwable t) {}
        try { Class.forName("android.os.Build$VERSION", true, null); } catch (Throwable t) {}
        try { Class.forName("android.os.Build$VERSION_CODES", true, null); } catch (Throwable t) {}
        log("[WL] Build pre-init done");
        ClassLoader cl = DynTest.class.getClassLoader();
        String[] tests = {
            "kotlin.Metadata",
            "dagger.hilt.android.HiltAndroidApp",
            "com.mcdonalds.sdk.connector.mcd.MCDConnector",
            "com.mcdonalds.mcdcoreapp.common.activity.SplashActivity",
            "com.mcdonalds.mcdcoreapp.presenter.SplashPresenterImpl",
            "com.mcdonalds.app.ordering.menu.MenuFragment",
            "com.mcdonalds.sdk.modules.models.MenuItem",
        };
        for (String cls : tests) {
            try {
                Class<?> c = Class.forName(cls, false, cl);
                String sup = "?";
                try { sup = shortName(c.getSuperclass().getName()); } catch (Throwable x) {}
                int mc = 0;
                try { mc = c.getDeclaredMethods().length; } catch (Throwable x) {}
                log("[WL] " + shortName(cls) + " extends " + sup + " (" + mc + " methods)");
            } catch (ClassNotFoundException e) {
                log("[WL] " + shortName(cls) + ": NOT FOUND");
            } catch (Throwable t) {
                log("[WL] " + shortName(cls) + ": " + shortName(t.getClass().getName()));
            }
        }
        // Show SplashActivity hierarchy
        try {
            Class<?> c = Class.forName("com.mcdonalds.mcdcoreapp.common.activity.SplashActivity", false, cl);
            log("[WL] --- Hierarchy ---");
            Class<?> cur = c;
            while (cur != null) { log("[WL]   " + cur.getName()); cur = cur.getSuperclass(); }
        } catch (Throwable t) {}
        log("[WL] DONE");
    }
}
