// Stub registers-aarch64.h for building Android 15 ART with Android 11 VIXL
// Android 15's code_generator_arm64.cc includes this header from newer VIXL.
// It doesn't exist in A11 VIXL. This stub provides the missing types.

#ifndef ART_STUBS_VIXL_REGISTERS_AARCH64_H_
#define ART_STUBS_VIXL_REGISTERS_AARCH64_H_

// Include A11 VIXL's operands header which has CPURegister, VRegister, etc.
#include "aarch64/operands-aarch64.h"

// The SVE types are injected by our macro-assembler-aarch64.h wrapper.
// This header just needs to exist so the #include doesn't fail.

#endif  // ART_STUBS_VIXL_REGISTERS_AARCH64_H_
