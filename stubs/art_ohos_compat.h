// art_ohos_compat.h - Pre-included header for A15 OHOS ARM64 cross-compilation
// Merges A11's OHOS compat header with A15's art_gcc_compat.h additions.

#ifndef ART_OHOS_COMPAT_H
#define ART_OHOS_COMPAT_H

// Force standard library headers that ART code assumes are available
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <signal.h>
#include <cstdint>
#include <climits>

// Prevent AOSP strlcpy.h from providing strlcpy (musl has it)
#define ART_LIBARTBASE_BASE_STRLCPY_H_

// ART image base address ASLR deltas (used by image_space.cc)
#ifndef ART_BASE_ADDRESS_MIN_DELTA
#define ART_BASE_ADDRESS_MIN_DELTA (-0x1000000)
#endif
#ifndef ART_BASE_ADDRESS_MAX_DELTA
#define ART_BASE_ADDRESS_MAX_DELTA 0x1000000
#endif

// ART frame size limit (used by instruction_set.cc)
#ifndef ART_FRAME_SIZE_LIMIT
#define ART_FRAME_SIZE_LIMIT 7400
#endif

// IMT (Interface Method Table) size (used by runtime headers)
#ifndef IMT_SIZE
#define IMT_SIZE 43
#endif

// ART compact DEX level (needed by dex2oat)
#ifndef ART_DEFAULT_COMPACT_DEX_LEVEL
#define ART_DEFAULT_COMPACT_DEX_LEVEL fast
#endif

// Enable all code generators (for cross-compilation support)
#ifndef ART_ENABLE_CODEGEN_arm
#define ART_ENABLE_CODEGEN_arm
#endif
#ifndef ART_ENABLE_CODEGEN_arm64
#define ART_ENABLE_CODEGEN_arm64
#endif
#ifndef ART_ENABLE_CODEGEN_x86
#define ART_ENABLE_CODEGEN_x86
#endif
#ifndef ART_ENABLE_CODEGEN_x86_64
#define ART_ENABLE_CODEGEN_x86_64
#endif
#ifndef ART_ENABLE_CODEGEN_riscv64
#define ART_ENABLE_CODEGEN_riscv64
#endif

// *** TARGET BUILD ***
// This is a target build (running on OHOS device), not a host build.
// We are NOT Android - we are OHOS with musl libc.
// Do NOT define __ANDROID__ or ART_TARGET_ANDROID.
#ifdef ART_TARGET
#undef ART_TARGET
#endif
#ifdef ART_TARGET_ANDROID
#undef ART_TARGET_ANDROID
#endif
#ifdef __ANDROID__
#undef __ANDROID__
#endif
#ifdef ANDROID
#undef ANDROID
#endif

// Android 15 uses HIDDEN attribute extensively: namespace art HIDDEN { ... }
// For standalone executable builds, override HIDDEN to default visibility.
// Hidden visibility causes "hidden symbol not defined" linker errors when
// linking compiler + runtime objects into a single dalvikvm executable.
#undef HIDDEN
#define HIDDEN /* default visibility for standalone build */
#undef ALWAYS_HIDDEN
#define ALWAYS_HIDDEN /* default visibility for standalone build */
#ifndef EXPORT
#define EXPORT __attribute__((visibility("default")))
#endif

// Android 15 ART metrics - stub the statsd reporting
#ifndef ART_METRICS_REPORTING_ENABLED
#define ART_METRICS_REPORTING_ENABLED 0
#endif

// Android 15 escape.h uses GCC 'typeof' extension; map to C++ decltype
#ifndef typeof
#define typeof(x) decltype(x)
#endif

// musl compatibility: sys/ucontext.h exists on musl but signal.h
// already includes the context structures we need.
#ifndef __MUSL_UCONTEXT_COMPAT__
#define __MUSL_UCONTEXT_COMPAT__
#endif

// musl does not provide getprogname() (BSD/bionic extension).
// Provide a minimal implementation using program_invocation_short_name (GNU extension).
#ifdef __MUSL__
#ifdef __cplusplus
extern "C" {
#endif
#include <errno.h>  // for program_invocation_short_name on some systems
static inline const char* getprogname(void) {
    extern char* program_invocation_short_name;
    return program_invocation_short_name ? program_invocation_short_name : "art";
}
#ifdef __cplusplus
}
#endif
#endif  // __MUSL__

#endif  // ART_OHOS_COMPAT_H
