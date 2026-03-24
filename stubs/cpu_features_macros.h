// Stub cpu_features_macros.h - from google/cpu_features library
// A15 uses this for runtime CPU feature detection
#ifndef ART_STUB_CPU_FEATURES_MACROS_H_
#define ART_STUB_CPU_FEATURES_MACROS_H_

// Define the architecture detection macros
#if defined(__x86_64__) || defined(_M_X64)
#define CPU_FEATURES_ARCH_X86_64
#define CPU_FEATURES_ARCH_X86
#elif defined(__aarch64__)
#define CPU_FEATURES_ARCH_AARCH64
#elif defined(__arm__)
#define CPU_FEATURES_ARCH_ARM
#elif defined(__riscv) && __riscv_xlen == 64
#define CPU_FEATURES_ARCH_RISCV
#endif

#define CPU_FEATURES_COMPILED_X86_AES 0
#define CPU_FEATURES_COMPILED_X86_AVX 0
#define CPU_FEATURES_COMPILED_X86_AVX2 0
#define CPU_FEATURES_COMPILED_X86_SSE4_1 0
#define CPU_FEATURES_COMPILED_X86_SSE4_2 0
#define CPU_FEATURES_COMPILED_X86_POPCNT 0

#endif  // ART_STUB_CPU_FEATURES_MACROS_H_
