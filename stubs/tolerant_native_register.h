// tolerant_native_register.h
// Force-included before all native/*.cc files.
// Intercepts RegisterNativeMethodsInternal by redefining CHECK_EQ
// specifically for the JNI RegisterNatives result check.
//
// Strategy: we cannot override CHECK_EQ globally (it's used everywhere).
// Instead, we define a wrapper macro that replaces the
// REGISTER_NATIVE_METHODS macro to use our tolerant version.

#ifndef TOLERANT_NATIVE_REGISTER_H_
#define TOLERANT_NATIVE_REGISTER_H_

// This header is -included BEFORE native_util.h is processed.
// We set a flag so we can override REGISTER_NATIVE_METHODS after
// native_util.h defines it.
#define ART_TOLERANT_REGISTER 1

#endif
