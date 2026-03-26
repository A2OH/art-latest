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
#endif

// art_quick_do_long_jump(gprs, fprs) - A11 assembly function
extern "C" void art_quick_do_long_jump(uintptr_t* gprs, uint64_t* fprs) __attribute__((noreturn));

namespace art HIDDEN {

// Replicate A11's X86_64Context::DoLongJump() logic for A15 Context objects.
// The A15 Context has the same gprs_/fprs_/rip_ layout as A11.
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
  // The PC to jump to. Use GetGPR on a non-standard "register" index won't work.
  // In A11/A15, the X86_64Context stores rip_ separately.
  // Access it via SmashCallerSaves pattern: RIP is stored in the context's rip_ field.
  // Unfortunately rip_ is private. But we can read it through the PC that was set.
  // The QuickExceptionHandler::PrepareLongJump() calls context->SetPC(handler_pc).
  // SetPC sets rip_. We need to read it back.
  // Since GetGPR/SetGPR doesn't cover RIP directly, and rip_ is protected/private,
  // we need a different approach. SetPC writes to a member variable.
  // For x86_64, context->GetPC() would give us the saved PC but there's no GetPC() in A15.
  //
  // Alternative: use the fact that A15 assembly gets the PC from Context->rip_.
  // In A15, art_quick_do_long_jump reads from the Context object directly.
  // In A11, DoLongJump() accesses rip_ directly (it's a member function).
  //
  // Since we can't access rip_ from outside, we'll read it from memory.
  // X86_64Context layout: inherits Context (vtable ptr = 8 bytes on x86_64),
  //   then gprs_[16] (16 * 8 = 128), fprs_[16] (16 * 8 = 128), rsp_ (8), rip_ (8), arg0_ (8)
  // Offset of rip_ from start of object: 8 (vtable) + 128 (gprs_) + 128 (fprs_) + 8 (rsp_) = 272
  uintptr_t* raw = reinterpret_cast<uintptr_t*>(ctx);
  // Safer: compute from OFFSETOF_MEMBER if available, or just hardcode for x86_64
  // sizeof(void*) [vtable] + 16*sizeof(uintptr_t*) [gprs_] + 16*sizeof(uint64_t*) [fprs_] + sizeof(uintptr_t) [rsp_]
  // = 8 + 128 + 128 + 8 = 272  on x86_64
  static constexpr size_t kRipOffset = 8 + 16 * 8 + 16 * 8 + 8;  // 272
  uintptr_t rip = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(ctx) + kRipOffset);

  *(reinterpret_cast<uintptr_t*>(rsp)) = rip;

  // Delete the context since we own it (was released from unique_ptr)
  // Actually we can't delete it here since we're about to long-jump away.
  // The memory will be leaked, which is acceptable for exception delivery.

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
