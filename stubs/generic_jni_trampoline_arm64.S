// Replacement art_quick_generic_jni_trampoline for ARM64
//
// This replaces the complex AOSP assembly trampoline with a minimal version
// that saves the quick calling convention registers, calls a C++ handler
// (artGenericJniTrampolineHandler) which does the entire native call
// internally using the interpreter path, and returns the result.
//
// The AOSP trampoline has issues with Thread structure offsets in our
// static build. By handling everything in C++, we avoid those issues.

#if defined(__aarch64__)

.text

// Override the A15 assembly version.
// Must be .globl (not .hidden) so the linker picks it up.
.type art_quick_generic_jni_trampoline, @function
.globl art_quick_generic_jni_trampoline
.balign 16
art_quick_generic_jni_trampoline:
    .cfi_startproc

    // On entry (ART quick calling convention):
    //   x0  = ArtMethod*
    //   x1-x7 = integer args
    //   d0-d7 = floating point args
    //   x19 = Thread* (xSELF)
    //   x30 = return address (LR)

    // Save frame pointer and link register
    stp     x29, x30, [sp, #-192]!
    .cfi_adjust_cfa_offset 192
    .cfi_rel_offset x29, 0
    .cfi_rel_offset x30, 8
    mov     x29, sp

    // Save ArtMethod* (x0), we need it for the C handler
    str     x0, [sp, #16]

    // Save integer arg registers x1-x7 to the stack buffer
    stp     x1, x2, [sp, #24]
    stp     x3, x4, [sp, #40]
    stp     x5, x6, [sp, #56]
    str     x7,     [sp, #72]

    // Save FP arg registers d0-d7 to the stack buffer
    stp     d0, d1, [sp, #80]
    stp     d2, d3, [sp, #96]
    stp     d4, d5, [sp, #112]
    stp     d6, d7, [sp, #128]

    // Save callee-saved x19 (Thread*) and x20 in case they're needed
    stp     x19, x20, [sp, #144]
    .cfi_rel_offset x19, 144
    .cfi_rel_offset x20, 152

    // Reserve space for result (16 bytes: uint64_t int_result, uint64_t fp_result)
    // at [sp, #160] and [sp, #168]

    // Call the C++ handler:
    // void artGenericJniTrampolineHandler(
    //     ArtMethod* method,       // x0 (already set)
    //     Thread* self,            // x1
    //     uint64_t* gpr_args,      // x2 (pointer to saved x1-x7)
    //     uint64_t* fpr_args,      // x3 (pointer to saved d0-d7)
    //     uint64_t* result_pair)   // x4 (pointer to [int_result, fp_result])
    //
    mov     x1, x19                // Thread* self
    add     x2, sp, #24            // gpr_args: pointer to saved x1-x7
    add     x3, sp, #80            // fpr_args: pointer to saved d0-d7
    add     x4, sp, #160           // result_pair: pointer to result buffer
    bl      artGenericJniTrampolineHandler

    // Load results
    ldr     x0, [sp, #160]         // Integer result
    ldr     x1, [sp, #168]         // FP result as raw bits
    fmov    d0, x1                 // Put FP result in d0

    // Restore callee-saved registers
    ldp     x19, x20, [sp, #144]
    .cfi_restore x19
    .cfi_restore x20

    // Restore frame pointer and link register, deallocate frame
    ldp     x29, x30, [sp], #192
    .cfi_adjust_cfa_offset -192
    .cfi_restore x29
    .cfi_restore x30

    ret

    .cfi_endproc
    .size art_quick_generic_jni_trampoline, .-art_quick_generic_jni_trampoline

#endif // __aarch64__

.section .note.GNU-stack,"",@progbits
