// Stub implementations for SVE vector code generator classes.
// These provide vtables for InstructionCodeGeneratorARM64Sve and
// LocationsBuilderARM64Sve which are referenced by code_generator_arm64.cc
// but whose implementations require Android 15's VIXL with full SVE support.
//
// The Visit* methods abort if called -- SVE codegen is not supported
// in this build. Non-SVE NEON vectorization still works.

#include "optimizing/code_generator_arm64.h"

namespace art HIDDEN {
namespace arm64 {

// ============= InstructionCodeGeneratorARM64Sve =============

#define IMPLEMENT_VISIT_STUB(name, super) \
void InstructionCodeGeneratorARM64Sve::Visit##name(H##name*) { \
  LOG(FATAL) << "SVE codegen not supported: " #name; \
}

FOR_EACH_CONCRETE_INSTRUCTION_VECTOR_COMMON(IMPLEMENT_VISIT_STUB)

#undef IMPLEMENT_VISIT_STUB

Location InstructionCodeGeneratorARM64Sve::AllocateSIMDScratchLocation(
    vixl::aarch64::UseScratchRegisterScope*) {
  LOG(FATAL) << "SVE codegen not supported";
  return Location();
}

void InstructionCodeGeneratorARM64Sve::FreeSIMDScratchLocation(
    Location, vixl::aarch64::UseScratchRegisterScope*) {
  LOG(FATAL) << "SVE codegen not supported";
}

void InstructionCodeGeneratorARM64Sve::LoadSIMDRegFromStack(Location, Location) {
  LOG(FATAL) << "SVE codegen not supported";
}

void InstructionCodeGeneratorARM64Sve::MoveSIMDRegToSIMDReg(Location, Location) {
  LOG(FATAL) << "SVE codegen not supported";
}

void InstructionCodeGeneratorARM64Sve::MoveToSIMDStackSlot(Location, Location) {
  LOG(FATAL) << "SVE codegen not supported";
}

void InstructionCodeGeneratorARM64Sve::SaveLiveRegistersHelper(LocationSummary*, int64_t) {
  LOG(FATAL) << "SVE codegen not supported";
}

void InstructionCodeGeneratorARM64Sve::RestoreLiveRegistersHelper(LocationSummary*, int64_t) {
  LOG(FATAL) << "SVE codegen not supported";
}

void InstructionCodeGeneratorARM64Sve::ValidateVectorLength(HVecOperation*) const {
  LOG(FATAL) << "SVE codegen not supported";
}

// ============= LocationsBuilderARM64Sve =============

#define IMPLEMENT_LOCATIONS_STUB(name, super) \
void LocationsBuilderARM64Sve::Visit##name(H##name*) { \
  LOG(FATAL) << "SVE codegen not supported: " #name; \
}

FOR_EACH_CONCRETE_INSTRUCTION_VECTOR_COMMON(IMPLEMENT_LOCATIONS_STUB)

#undef IMPLEMENT_LOCATIONS_STUB

}  // namespace arm64
}  // namespace art
