// Shadow runtime.h - suppress non-standard offsetof usage in constexpr function
// The real runtime.h has GetCalleeSaveMethodOffset() which uses:
//   OFFSETOF_MEMBER(Runtime, callee_save_methods_[static_cast<size_t>(type)])
// This is a variable-index offsetof, which is non-standard but should be accepted
// by clang as a system header (warnings/errors suppressed by #pragma GCC system_header).
//
// CRITICAL: We must NOT redefine OFFSETOF_MEMBER, as that would break ALL field
// offset computations throughout the entire transitive include chain (DexCache,
// Class, etc.), causing reads at offset 0 instead of the correct field offsets.
#ifndef STUB_RUNTIME_WRAPPER_H_
#define STUB_RUNTIME_WRAPPER_H_

#pragma GCC system_header
#include_next "runtime.h"

#endif  // STUB_RUNTIME_WRAPPER_H_
