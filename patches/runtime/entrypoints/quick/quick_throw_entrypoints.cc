/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Patched for Westlake standalone builds:
 * A15 quick_throw_entrypoints return Context* for the assembly caller to do the long jump.
 * But we use A11 assembly which expects these functions to NEVER RETURN (the A11 assembly
 * puts int3 right after the call). To bridge the gap, we replicate A11's DoLongJump()
 * logic: fill temporary register arrays from the Context, then call art_quick_do_long_jump.
 */

#include "arch/context.h"
#include "art_method-inl.h"
#include "callee_save_frame.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_instruction-inl.h"
#include "common_throws.h"
#include "mirror/object-inl.h"
#include "nth_caller_visitor.h"
#include "thread.h"
#include "well_known_classes.h"

#if defined(__x86_64__)
#include "arch/x86_64/context_x86_64.h"
using art::x86_64::X86_64Context;
using art::x86_64::kNumberOfCpuRegisters;
using art::x86_64::kNumberOfFloatRegisters;
using art::x86_64::RSP;
#elif defined(__aarch64__)
#include "arch/arm64/context_arm64.h"
using art::arm64::Arm64Context;
using art::arm64::kNumberOfXRegisters;
using art::arm64::kNumberOfDRegisters;
#endif

// art_quick_do_long_jump(gprs, fprs) - A11 assembly function
extern "C" void art_quick_do_long_jump(uintptr_t* gprs, uint64_t* fprs) __attribute__((noreturn));

namespace art HIDDEN {

// Replicate A11's Context::DoLongJump() logic for A15 Context objects.
// A15 throw entrypoints return Context*; A11 assembly expects them to never return.
// We bridge the gap by extracting registers into flat arrays and calling
// art_quick_do_long_jump from A11's assembly.
static void DoContextLongJump(Context* context) __attribute__((noreturn));
static void DoContextLongJump(Context* context) {
#if defined(__x86_64__)
  // Cast to X86_64Context to access internal fields
  X86_64Context* ctx = static_cast<X86_64Context*>(context);

  // Access the internal arrays via the public API
  uintptr_t gprs[kNumberOfCpuRegisters + 1];
  uint64_t fprs[kNumberOfFloatRegisters];

  for (size_t i = 0; i < kNumberOfCpuRegisters; ++i) {
    if (ctx->IsAccessibleGPR(i)) {
      gprs[kNumberOfCpuRegisters - i - 1] = ctx->GetGPR(i);
    } else {
      gprs[kNumberOfCpuRegisters - i - 1] = 0xDEAD0000 + i;  // Bad GPR sentinel
    }
  }
  for (size_t i = 0; i < kNumberOfFloatRegisters; ++i) {
    if (ctx->IsAccessibleFPR(i)) {
      fprs[i] = ctx->GetFPR(i);
    } else {
      fprs[i] = 0xDEAD0000 + kNumberOfCpuRegisters + i;  // Bad FPR sentinel
    }
  }

  // Set up RSP: point one slot below so that the ret in art_quick_do_long_jump
  // will pop the return address (RIP).
  uintptr_t rsp = gprs[kNumberOfCpuRegisters - RSP - 1] - sizeof(intptr_t);
  gprs[kNumberOfCpuRegisters] = rsp;

  // Read rip_ from Context memory layout.
  // X86_64Context layout: vtable(8) + gprs_[16](128) + fprs_[16](128) + rsp_(8) = 272
  static constexpr size_t kRipOffset = 8 + 16 * 8 + 16 * 8 + 8;  // 272
  uintptr_t rip = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(ctx) + kRipOffset);

  *(reinterpret_cast<uintptr_t*>(rsp)) = rip;

  art_quick_do_long_jump(gprs, fprs);
  __builtin_unreachable();

#elif defined(__aarch64__)
  // ARM64: Arm64Context uses pointer-based gprs_/fprs_ arrays.
  // gprs_[kNumberOfXRegisters + 1] = 34 pointers (33 X-regs + 1 for PC)
  // fprs_[kNumberOfDRegisters] = 32 pointers (32 D-regs)
  // art_quick_do_long_jump expects flat value arrays:
  //   x0 = gprs[34] (values), x1 = fprs[32] (values)
  //   gprs[31] = SP, gprs[33] = PC
  Arm64Context* ctx = static_cast<Arm64Context*>(context);

  // kNumberOfXRegisters = 33, kPC = 33
  static constexpr size_t kPC = kNumberOfXRegisters;
  uintptr_t gprs[kNumberOfXRegisters + 1];  // 34 entries: x0-x32 + PC
  uint64_t fprs[kNumberOfDRegisters];        // 32 entries: d0-d31

  for (size_t i = 0; i < kNumberOfXRegisters; ++i) {
    if (ctx->IsAccessibleGPR(i)) {
      gprs[i] = ctx->GetGPR(i);
    } else {
      gprs[i] = 0xDEAD0000 + i;
    }
  }
  // PC is stored at index kPC (33) in gprs_ — read via memory since
  // GetGPR doesn't allow reading the PC register.
  // Arm64Context layout:
  //   vtable(8) + gprs_[34](272) + fprs_[32](256) + sp_(8) + pc_(8) + arg0_(8)
  // gprs_ starts at offset 8, PC pointer is at gprs_[33] = offset 8 + 33*8 = 272
  // But gprs_ stores POINTERS. We need the pointed-to value.
  // The PC was set via SetPC -> SetGPR(kPC, value) which sets gprs_[kPC] = &pc_
  // and pc_ = value. The pc_ field is at offset:
  //   8 (vtable) + 34*8 (gprs_) + 32*8 (fprs_) + 8 (sp_) = 8 + 272 + 256 + 8 = 544
  static constexpr size_t kPcFieldOffset = 8 + (kNumberOfXRegisters + 1) * 8 + kNumberOfDRegisters * 8 + 8;
  gprs[kPC] = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(ctx) + kPcFieldOffset);

  for (size_t i = 0; i < kNumberOfDRegisters; ++i) {
    if (ctx->IsAccessibleFPR(i)) {
      fprs[i] = ctx->GetFPR(i);
    } else {
      fprs[i] = 0;
    }
  }

  art_quick_do_long_jump(gprs, fprs);
  __builtin_unreachable();

#else
  (void)context;
  abort();
#endif
}

// Helper: deliver exception and do the long jump (never returns).
static void DeliverAndJump(Thread* self) __attribute__((noreturn));
static void DeliverAndJump(Thread* self) {
  std::unique_ptr<Context> context = self->QuickDeliverException();
  DCHECK(context != nullptr);
  DoContextLongJump(context.release());
}

// Deliver an exception that's pending on thread helping set up a callee save frame on the way.
extern "C" NO_RETURN void artDeliverPendingExceptionFromCode(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  DeliverAndJump(self);
}

extern "C" NO_RETURN void artInvokeObsoleteMethod(ArtMethod* method, Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(method->IsObsolete());
  ScopedQuickEntrypointChecks sqec(self);
  ThrowInternalError("Attempting to invoke obsolete version of '%s'.",
                     method->PrettyMethod().c_str());
  DeliverAndJump(self);
}

// Called by generated code to throw an exception.
extern "C" NO_RETURN void artDeliverExceptionFromCode(mirror::Throwable* exception, Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  if (exception == nullptr) {
    self->ThrowNewException("Ljava/lang/NullPointerException;", nullptr);
  } else {
    self->SetException(exception);
  }
  DeliverAndJump(self);
}

// Called by generated code to throw a NPE exception.
extern "C" NO_RETURN void artThrowNullPointerExceptionFromCode(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  ThrowNullPointerExceptionFromDexPC(/* check_address= */ false, 0U);
  DeliverAndJump(self);
}

// Installed by a signal handler to throw a NPE exception.
extern "C" NO_RETURN void artThrowNullPointerExceptionFromSignal(uintptr_t addr, Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  ThrowNullPointerExceptionFromDexPC(/* check_address= */ true, addr);
  DeliverAndJump(self);
}

// Called by generated code to throw an arithmetic divide by zero exception.
extern "C" NO_RETURN void artThrowDivZeroFromCode(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  ThrowArithmeticExceptionDivideByZero();
  DeliverAndJump(self);
}

// Called by generated code to throw an array index out of bounds exception.
extern "C" NO_RETURN void artThrowArrayBoundsFromCode(int index, int length, Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  ThrowArrayIndexOutOfBoundsException(index, length);
  DeliverAndJump(self);
}

// Called by generated code to throw a string index out of bounds exception.
extern "C" NO_RETURN void artThrowStringBoundsFromCode(int index, int length, Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  ThrowStringIndexOutOfBoundsException(index, length);
  DeliverAndJump(self);
}

extern "C" NO_RETURN void artThrowStackOverflowFromCode(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  ThrowStackOverflowError(self);
  DeliverAndJump(self);
}

extern "C" NO_RETURN void artThrowClassCastException(mirror::Class* dest_type,
                                                     mirror::Class* src_type,
                                                     Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  if (dest_type == nullptr) {
    NthCallerVisitor visitor(self, 0u);
    visitor.WalkStack();
    DCHECK(visitor.caller != nullptr);
    uint32_t dex_pc = visitor.GetDexPc();
    CodeItemDataAccessor accessor(*visitor.caller->GetDexFile(), visitor.caller->GetCodeItem());
    const Instruction& check_cast = accessor.InstructionAt(dex_pc);
    DCHECK_EQ(check_cast.Opcode(), Instruction::CHECK_CAST);
    dex::TypeIndex type_index(check_cast.VRegB_21c());
    ClassLinker* linker = Runtime::Current()->GetClassLinker();
    dest_type = linker->LookupResolvedType(type_index, visitor.caller).Ptr();
    CHECK(dest_type != nullptr) << "Target class should have been previously resolved: "
        << visitor.caller->GetDexFile()->PrettyType(type_index);
    CHECK(!dest_type->IsAssignableFrom(src_type));
  }
  DCHECK(!dest_type->IsAssignableFrom(src_type));
  ThrowClassCastException(dest_type, src_type);
  DeliverAndJump(self);
}

extern "C" NO_RETURN void artThrowClassCastExceptionForObject(mirror::Object* obj,
                                                              mirror::Class* dest_type,
                                                              Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(obj != nullptr);
  artThrowClassCastException(dest_type, obj->GetClass(), self);
}

extern "C" NO_RETURN void artThrowArrayStoreException(mirror::Object* array,
                                                      mirror::Object* value,
                                                      Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  ThrowArrayStoreException(value->GetClass(), array->GetClass());
  DeliverAndJump(self);
}

}  // namespace art
