// Stub cpuinfo_aarch64.h - from google/cpu_features library
// A15 uses cpu_features::GetAarch64Info() (C++ namespace)
#ifndef ART_STUB_CPUINFO_AARCH64_H_
#define ART_STUB_CPUINFO_AARCH64_H_

#include "cpu_features_macros.h"

namespace cpu_features {

typedef struct {
  int fp;
  int asimd;
  int aes;
  int pmull;
  int sha1;
  int sha2;
  int crc32;
  int atomics;
  int fphp;
  int asimdhp;
  int asimddp;
  int sve;
  int dotprod;
} Aarch64Features;

typedef struct {
  Aarch64Features features;
} Aarch64Info;

static inline Aarch64Info GetAarch64Info() {
  Aarch64Info info = {};
  return info;
}

}  // namespace cpu_features

#endif  // ART_STUB_CPUINFO_AARCH64_H_
