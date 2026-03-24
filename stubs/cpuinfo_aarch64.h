// Stub cpuinfo_aarch64.h - from google/cpu_features library
#ifndef ART_STUB_CPUINFO_AARCH64_H_
#define ART_STUB_CPUINFO_AARCH64_H_

#include "cpu_features_macros.h"

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
  int sve;
  int dotprod;
} Aarch64Features;

typedef struct {
  Aarch64Features features;
} Aarch64Info;

static inline Aarch64Info GetAarch64Info(void) {
  Aarch64Info info = {};
  return info;
}

#endif  // ART_STUB_CPUINFO_AARCH64_H_
