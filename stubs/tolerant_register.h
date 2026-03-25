// tolerant_register.h - Override RegisterNativeMethodsInternal to be tolerant
// Include this BEFORE native_util.h via -include flag
// This defines the function before native_util.h's inline version,
// effectively making the inline version a no-op redefinition.
//
// Actually, we use a different approach: define a macro that replaces
// RegisterNativeMethodsInternal with our tolerant version.

#ifndef TOLERANT_REGISTER_H_
#define TOLERANT_REGISTER_H_

// We'll redefine REGISTER_NATIVE_METHODS after native_util.h is included.
// Mark that we want tolerant behavior.
#define ART_USE_TOLERANT_REGISTER_NATIVES 1

#endif
