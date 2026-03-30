package art.io;

import java.io.IOException;
import java.io.OutputStream;
import java.io.Writer;

/**
 * Writes UTF-8 bytes directly to fd via native write(), bypassing
 * FileOutputStream/IoTracker/BlockGuard/ThreadLocal entirely.
 */
public class Utf8Writer extends Writer {
    private final OutputStream out;
    private int fd;
    private final byte[] buf = new byte[8192];
    private int pos = 0;

    // Native write — registered from dalvikvm.cc JNI
    static native int nativeWrite(int fd, byte[] data, int off, int len);

    public Utf8Writer(OutputStream out) {
        super(out);
        this.out = out;
        this.fd = -1;
    }

    public void setFd(int rawFd) { this.fd = rawFd; }

    private void putByte(int b) throws IOException {
        if (pos >= buf.length) drainBuffer();
        buf[pos++] = (byte)b;
    }

    private void drainBuffer() throws IOException {
        if (pos > 0) {
            if (fd >= 0) {
                nativeWrite(fd, buf, 0, pos);
            } else {
                out.write(buf, 0, pos);
            }
            pos = 0;
        }
    }

    public void write(int c) throws IOException {
        synchronized (out) {
            if (c < 0x80) putByte(c);
            else if (c < 0x800) { putByte(0xC0|(c>>6)); putByte(0x80|(c&0x3F)); }
            else { putByte(0xE0|(c>>12)); putByte(0x80|((c>>6)&0x3F)); putByte(0x80|(c&0x3F)); }
        }
    }

    public void write(char[] cbuf, int off, int len) throws IOException {
        synchronized (out) {
            for (int i = off, end = off+len; i < end; i++) {
                char c = cbuf[i];
                if (c < 0x80) putByte(c);
                else if (c < 0x800) { putByte(0xC0|(c>>6)); putByte(0x80|(c&0x3F)); }
                else { putByte(0xE0|(c>>12)); putByte(0x80|((c>>6)&0x3F)); putByte(0x80|(c&0x3F)); }
            }
            drainBuffer();
        }
    }

    public void write(String str, int off, int len) throws IOException {
        synchronized (out) {
            for (int i = off, end = off+len; i < end; i++) {
                char c = str.charAt(i);
                if (c < 0x80) putByte(c);
                else if (c < 0x800) { putByte(0xC0|(c>>6)); putByte(0x80|(c&0x3F)); }
                else { putByte(0xE0|(c>>12)); putByte(0x80|((c>>6)&0x3F)); putByte(0x80|(c&0x3F)); }
            }
            drainBuffer();
        }
    }

    public void flush() throws IOException {
        synchronized (out) { drainBuffer(); }
    }

    public void close() throws IOException {
        synchronized (out) { drainBuffer(); }
    }
}
