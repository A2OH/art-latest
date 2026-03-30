package art.io;

import java.io.OutputStream;
import java.io.IOException;

/**
 * OutputStream that writes directly to a file descriptor via JNI,
 * bypassing FileOutputStream/IoTracker/BlockGuard/ThreadLocal.
 */
public class NativeOutputStream extends OutputStream {
    private final int fd;

    public NativeOutputStream(int fd) {
        this.fd = fd;
    }

    public void write(int b) throws IOException {
        Utf8Writer.nativeWrite(fd, new byte[]{(byte)b}, 0, 1);
    }

    public void write(byte[] b, int off, int len) throws IOException {
        Utf8Writer.nativeWrite(fd, b, off, len);
    }

    public void flush() {} // no-op
    public void close() {} // no-op
}
