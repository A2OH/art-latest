/*
 * Portable java.lang.Character native subset for Westlake's imageless runtime.
 *
 * The full Android implementation delegates to ICU. Westlake must not jump
 * through an unregistered/null Character native entrypoint while bootstrapping
 * stock APKs, so this file registers a conservative ASCII-first subset. The
 * interpreter still has a matching fast-path; these JNI registrations cover
 * direct/native dispatch and any call site the interpreter fast-path misses.
 */

#include "nativehelper/jni_macros.h"

#include <cstdio>
#include <jni.h>

#include "tolerant_native_util.h"

namespace art HIDDEN {

static constexpr jint kUnassigned = 0;
static constexpr jint kUppercaseLetter = 1;
static constexpr jint kLowercaseLetter = 2;
static constexpr jint kTitlecaseLetter = 3;
static constexpr jint kOtherLetter = 5;
static constexpr jint kDecimalDigitNumber = 9;
static constexpr jint kSpaceSeparator = 12;
static constexpr jint kControl = 15;
static constexpr jint kOtherPunctuation = 24;
static constexpr jint kMathSymbol = 25;

static bool IsAsciiLower(jint cp) {
  return cp >= 'a' && cp <= 'z';
}

static bool IsAsciiUpper(jint cp) {
  return cp >= 'A' && cp <= 'Z';
}

static bool IsAsciiDigit(jint cp) {
  return cp >= '0' && cp <= '9';
}

static bool IsAsciiLetter(jint cp) {
  return IsAsciiLower(cp) || IsAsciiUpper(cp);
}

static bool IsAsciiWhitespace(jint cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' ||
         cp == '\r' || cp == '\f' || cp == 0x0b;
}

static bool IsAsciiIdentifierStart(jint cp) {
  return IsAsciiLetter(cp) || cp == '_' || cp == '$';
}

static bool IsAsciiIdentifierPart(jint cp) {
  return IsAsciiIdentifierStart(cp) || IsAsciiDigit(cp);
}

static jboolean Character_isLowerCaseImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiLower(cp) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isUpperCaseImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiUpper(cp) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isTitleCaseImpl(JNIEnv*, jclass, jint cp) {
  return JNI_FALSE;
}

static jboolean Character_isDigitImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiDigit(cp) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isDefinedImpl(JNIEnv*, jclass, jint cp) {
  return (cp >= 0 && cp <= 0x10ffff) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isLetterImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiLetter(cp) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isLetterOrDigitImpl(JNIEnv*, jclass, jint cp) {
  return (IsAsciiLetter(cp) || IsAsciiDigit(cp)) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isAlphabeticImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiLetter(cp) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isIdeographicImpl(JNIEnv*, jclass, jint cp) {
  return JNI_FALSE;
}

static jboolean Character_isUnicodeIdentifierStartImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiIdentifierStart(cp) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isUnicodeIdentifierPartImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiIdentifierPart(cp) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isIdentifierIgnorableImpl(JNIEnv*, jclass, jint cp) {
  return ((cp >= 0x0000 && cp <= 0x0008) ||
          (cp >= 0x000e && cp <= 0x001b) ||
          (cp >= 0x007f && cp <= 0x009f)) ? JNI_TRUE : JNI_FALSE;
}

static jint Character_toLowerCaseImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiUpper(cp) ? cp + ('a' - 'A') : cp;
}

static jint Character_toUpperCaseImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiLower(cp) ? cp - ('a' - 'A') : cp;
}

static jint Character_toTitleCaseImpl(JNIEnv*, jclass, jint cp) {
  return Character_toUpperCaseImpl(nullptr, nullptr, cp);
}

static jint Character_digitImpl(JNIEnv*, jclass, jint cp, jint radix) {
  jint value = -1;
  if (IsAsciiDigit(cp)) {
    value = cp - '0';
  } else if (IsAsciiUpper(cp)) {
    value = cp - 'A' + 10;
  } else if (IsAsciiLower(cp)) {
    value = cp - 'a' + 10;
  }
  return (radix >= 2 && radix <= 36 && value >= 0 && value < radix) ? value : -1;
}

static jint Character_getNumericValueImpl(JNIEnv*, jclass, jint cp) {
  if (IsAsciiDigit(cp)) {
    return cp - '0';
  }
  if (IsAsciiUpper(cp)) {
    return cp - 'A' + 10;
  }
  if (IsAsciiLower(cp)) {
    return cp - 'a' + 10;
  }
  return -1;
}

static jboolean Character_isSpaceCharImpl(JNIEnv*, jclass, jint cp) {
  return (cp == ' ') ? JNI_TRUE : JNI_FALSE;
}

static jboolean Character_isWhitespaceImpl(JNIEnv*, jclass, jint cp) {
  return IsAsciiWhitespace(cp) ? JNI_TRUE : JNI_FALSE;
}

static jint Character_getTypeImpl(JNIEnv*, jclass, jint cp) {
  if (IsAsciiUpper(cp)) {
    return kUppercaseLetter;
  }
  if (IsAsciiLower(cp)) {
    return kLowercaseLetter;
  }
  if (IsAsciiDigit(cp)) {
    return kDecimalDigitNumber;
  }
  if (cp == ' ') {
    return kSpaceSeparator;
  }
  if ((cp >= 0 && cp < 0x20) || cp == 0x7f) {
    return kControl;
  }
  if (cp == '+' || cp == '-' || cp == '*' || cp == '/' || cp == '=' ||
      cp == '<' || cp == '>' || cp == '%') {
    return kMathSymbol;
  }
  if (cp >= 0x80 && cp <= 0x10ffff) {
    return kOtherLetter;
  }
  if (cp >= 0) {
    return kOtherPunctuation;
  }
  return kUnassigned;
}

static jbyte Character_getDirectionalityImpl(JNIEnv*, jclass, jint) {
  return 0;  // DIRECTIONALITY_LEFT_TO_RIGHT
}

static jboolean Character_isMirroredImpl(JNIEnv*, jclass, jint) {
  return JNI_FALSE;
}

static jstring Character_getNameImpl(JNIEnv* env, jclass, jint cp) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "U+%04X", static_cast<unsigned int>(cp));
  return env->NewStringUTF(buffer);
}

static jint Character_codePointOfImpl(JNIEnv*, jclass, jstring) {
  return -1;
}

static JNINativeMethod gMethods[] = {
    FAST_NATIVE_METHOD(Character, isLowerCaseImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isUpperCaseImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isTitleCaseImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isDigitImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isDefinedImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isLetterImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isLetterOrDigitImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isAlphabeticImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isIdeographicImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isUnicodeIdentifierStartImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isUnicodeIdentifierPartImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isIdentifierIgnorableImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, toLowerCaseImpl, "(I)I"),
    FAST_NATIVE_METHOD(Character, toUpperCaseImpl, "(I)I"),
    FAST_NATIVE_METHOD(Character, toTitleCaseImpl, "(I)I"),
    FAST_NATIVE_METHOD(Character, digitImpl, "(II)I"),
    FAST_NATIVE_METHOD(Character, getNumericValueImpl, "(I)I"),
    FAST_NATIVE_METHOD(Character, isSpaceCharImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, isWhitespaceImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, getTypeImpl, "(I)I"),
    FAST_NATIVE_METHOD(Character, getDirectionalityImpl, "(I)B"),
    FAST_NATIVE_METHOD(Character, isMirroredImpl, "(I)Z"),
    FAST_NATIVE_METHOD(Character, getNameImpl, "(I)Ljava/lang/String;"),
    FAST_NATIVE_METHOD(Character, codePointOfImpl, "(Ljava/lang/String;)I"),
};

void register_java_lang_Character(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Character");
}

}  // namespace art
