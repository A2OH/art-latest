// vixl_sve_compat.h - SVE type stubs for building Android 15 ART with Android 11 VIXL
// Android 15's ART compiler references VIXL SVE types (ZRegister, SVEMemOperand, etc.)
// that don't exist in Android 11's VIXL. This header provides minimal stubs.

#ifndef ART_STUBS_VIXL_SVE_COMPAT_H_
#define ART_STUBS_VIXL_SVE_COMPAT_H_

#include "aarch64/operands-aarch64.h"

namespace vixl {
namespace aarch64 {

// SVE addressing mode modifier
enum SVEOffsetModifier {
  SVE_MUL_VL = 0
};

// DRegister: 64-bit FP/SIMD register (newer VIXL has this as a separate type)
inline VRegister DRegister(unsigned code) {
  return VRegister(code, kDRegSize);
}

// SRegister: 32-bit FP/SIMD register
inline VRegister SRegister(unsigned code) {
  return VRegister(code, kSRegSize);
}

// ZRegister: SVE scalable vector register (stub - returns a 128-bit VRegister)
// This is only used in SaveRestoreZRegisterList which won't be called on non-SVE targets
inline VRegister ZRegister(unsigned code) {
  return VRegister(code, kQRegSize);
}

// SVEMemOperand: SVE memory operand (stub)
// Only used in SaveRestoreZRegisterList for SVE spill/fill
class SVEMemOperand {
 public:
  SVEMemOperand(const Register& base, int offset, SVEOffsetModifier mod = SVE_MUL_VL)
      : base_(base), offset_(offset) {}
  const Register& GetBase() const { return base_; }
  int GetOffset() const { return offset_; }
 private:
  Register base_;
  int offset_;
};

}  // namespace aarch64
}  // namespace vixl

#endif  // ART_STUBS_VIXL_SVE_COMPAT_H_
