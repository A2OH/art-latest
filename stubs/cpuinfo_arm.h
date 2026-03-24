// Stub cpuinfo_arm.h - from google/cpu_features library
#ifndef ART_STUB_CPUINFO_ARM_H_
#define ART_STUB_CPUINFO_ARM_H_

#include "cpu_features_macros.h"

typedef struct {
  int neon;
  int vfpv3;
  int vfpv4;
  int idiva;
  int aes;
} ArmFeatures;

typedef struct {
  ArmFeatures features;
} ArmInfo;

static inline ArmInfo GetArmInfo(void) {
  ArmInfo info = {};
  return info;
}

#endif  // ART_STUB_CPUINFO_ARM_H_
