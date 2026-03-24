// Wrapper for VIXL macro-assembler-aarch64.h that adds SVE type stubs
// Android 15 ART uses SVE types from a newer VIXL; Android 11 VIXL lacks them.
// This wrapper includes the real header then injects minimal SVE compatibility types.

#ifndef ART_STUBS_VIXL_MACRO_ASSEMBLER_WRAPPER_H_
#define ART_STUBS_VIXL_MACRO_ASSEMBLER_WRAPPER_H_

// Include the real VIXL header by chaining to the next in the include path.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wgnu-include-next"
#include_next "aarch64/macro-assembler-aarch64.h"
#pragma GCC diagnostic pop

// Inject SVE compatibility types into the vixl::aarch64 namespace.
// These are stubs -- they compile but SVE code paths should not be executed
// at runtime (SVE codegen is optional; non-SVE ARM64 codegen is the default).

namespace vixl {
namespace aarch64 {

// SVE addressing mode modifier
enum SVEOffsetModifier {
  SVE_MUL_VL = 0
};

// ZRegister: SVE scalable vector register
// In newer VIXL this inherits from CPURegister; here we use VRegister as base
// since A11 VIXL doesn't have kVRegisterBank.
class ZRegister : public VRegister {
 public:
  explicit ZRegister(int code, int lane_size = 0)
      : VRegister(code, kQRegSize) {}

  ZRegister VnB() const { return ZRegister(GetCode()); }
  ZRegister VnH() const { return ZRegister(GetCode()); }
  ZRegister VnS() const { return ZRegister(GetCode()); }
  ZRegister VnD() const { return ZRegister(GetCode()); }
  ZRegister VnQ() const { return ZRegister(GetCode()); }

  template <typename T>
  ZRegister WithLaneSize(T format) const { return ZRegister(GetCode()); }
  ZRegister WithSameLaneSizeAs(const CPURegister& other) const { return ZRegister(GetCode()); }
};

// PRegister: SVE predicate register
// A11 VIXL has no kPRegister type, so we use a minimal wrapper around CPURegister.
class PRegister : public CPURegister {
 public:
  explicit PRegister(int code = 0) : CPURegister(code, kXRegSize, kRegister) {}

  bool IsValid() const { return true; }

  // Stub methods that newer VIXL PRegister provides
  PRegister Zeroing() const { return *this; }
  PRegister Merging() const { return *this; }
};

class PRegisterWithLaneSize : public PRegister {
 public:
  explicit PRegisterWithLaneSize(int code = 0) : PRegister(code) {}
};

class PRegisterZ : public PRegister {
 public:
  explicit PRegisterZ(int code = 0) : PRegister(code) {}
};

class PRegisterM : public PRegister {
 public:
  explicit PRegisterM(int code = 0) : PRegister(code) {}
};

// Predicate register constants p0-p15 (inline to avoid ODR violations)
inline const PRegister& GetP0() { static const PRegister r(0); return r; }
inline const PRegister& GetP1() { static const PRegister r(1); return r; }
inline const PRegister& GetP2() { static const PRegister r(2); return r; }
inline const PRegister& GetP3() { static const PRegister r(3); return r; }
inline const PRegister& GetP4() { static const PRegister r(4); return r; }
inline const PRegister& GetP5() { static const PRegister r(5); return r; }
inline const PRegister& GetP6() { static const PRegister r(6); return r; }
inline const PRegister& GetP7() { static const PRegister r(7); return r; }

// Use inline variables (C++17) to avoid multiple-definition linker errors
inline const PRegister p0(0);
inline const PRegister p1(1);
inline const PRegister p2(2);
inline const PRegister p3(3);
inline const PRegister p4(4);
inline const PRegister p5(5);
inline const PRegister p6(6);
inline const PRegister p7(7);
inline const PRegister p8(8);
inline const PRegister p9(9);
inline const PRegister p10(10);
inline const PRegister p11(11);
inline const PRegister p12(12);
inline const PRegister p13(13);
inline const PRegister p14(14);
inline const PRegister p15(15);

// Sized FP register constructors (newer VIXL has these; A11 doesn't)
inline VRegister BRegister(unsigned code) { return VRegister(code, kBRegSize); }
inline VRegister HRegister(unsigned code) { return VRegister(code, kHRegSize); }
inline VRegister SRegister(unsigned code) { return VRegister(code, kSRegSize); }
inline VRegister DRegister(unsigned code) { return VRegister(code, kDRegSize); }
inline VRegister QRegister(unsigned code) { return VRegister(code, kQRegSize); }

// SVEMemOperand: SVE memory operand (stub)
// Supports (base, offset), (base, offset, mod), and (base, index, shift, amount) forms.
class SVEMemOperand : public MemOperand {
 public:
  SVEMemOperand(const Register& base, int64_t offset)
      : MemOperand(base, offset) {}
  SVEMemOperand(const Register& base, int64_t offset, SVEOffsetModifier mod)
      : MemOperand(base, offset) {}
  SVEMemOperand(const Register& base, const Register& index, Shift shift, unsigned amount)
      : MemOperand(base, index, shift, amount) {}
};

}  // namespace aarch64
}  // namespace vixl

#endif  // ART_STUBS_VIXL_MACRO_ASSEMBLER_WRAPPER_H_
