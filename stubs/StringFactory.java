package java.lang;

public final class StringFactory {
    public static native String newStringFromBytes(byte[] data, int high, int offset, int byteCount);
    public static native String newStringFromChars(int offset, int charCount, char[] data);
    public static native String newStringFromString(String toCopy);
    public static native String newStringFromUtf8Bytes(byte[] data, int offset, int byteCount);
    public static native String newStringFromUtf16Bytes(byte[] data, int offset, int byteCount);
}
