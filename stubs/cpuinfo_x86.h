// Stub cpuinfo_x86.h - from google/cpu_features library
#ifndef ART_STUB_CPUINFO_X86_H_
#define ART_STUB_CPUINFO_X86_H_

#include "cpu_features_macros.h"

namespace cpu_features {

typedef struct {
  int sse;
  int sse2;
  int sse3;
  int ssse3;
  int sse4_1;
  int sse4_2;
  int avx;
  int avx2;
  int avx512f;
  int aes;
  int popcnt;
  int cx16;
} X86Features;

typedef struct {
  X86Features features;
} X86Info;

static inline X86Info GetX86Info(void) {
  X86Info info = {};
  return info;
}

}  // namespace cpu_features

#endif  // ART_STUB_CPUINFO_X86_H_
