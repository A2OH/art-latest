// Stub assembly entry points for Android 15 ART symbols not in Android 11's
// quick_entrypoints_x86_64.S. These are needed to satisfy link-time references.

#if defined(__x86_64__)

.macro DEFINE_FUNCTION name
    .type \name, @function
    .hidden \name
    .globl \name
    .balign 16
\name:
.endm

.macro END_FUNCTION name
    .size \name, .-\name
.endm

// art_quick_method_exit_hook - instrumentation hook for method exit
// Added in Android 15. Only called during runtime profiling/tracing.
DEFINE_FUNCTION art_quick_method_exit_hook
    ret    // Should never be called in dex2oat
    ret
END_FUNCTION art_quick_method_exit_hook

// art_quick_method_entry_hook - instrumentation hook for method entry
DEFINE_FUNCTION art_quick_method_entry_hook
    ret
    ret
END_FUNCTION art_quick_method_entry_hook

// Nterp interpreter stubs - these are the assembly entry points referenced by nterp.cc.
// dex2oat never actually executes nterp (CanRuntimeUseNterp returns false for AOT compiler),
// but nterp.cc references these symbols for GetNterpEntryPoint/NterpImpl/NterpWithClinitImpl.
// We provide minimal stubs so the size computation doesn't crash.

.macro DEFINE_PROTECTED name
    .type \name, @function
    .protected \name
    .globl \name
    .balign 16
\name:
.endm

DEFINE_PROTECTED ExecuteNterpImpl
    ret
    ret
END_FUNCTION ExecuteNterpImpl

DEFINE_PROTECTED EndExecuteNterpImpl
    nop
END_FUNCTION EndExecuteNterpImpl

DEFINE_PROTECTED ExecuteNterpWithClinitImpl
    ret
    ret
END_FUNCTION ExecuteNterpWithClinitImpl

DEFINE_PROTECTED EndExecuteNterpWithClinitImpl
    nop
END_FUNCTION EndExecuteNterpWithClinitImpl

// artNterpAsmInstructionStart/End - used by CheckNterpAsmConstants to compute
// interpreter handler table size. We make them equal so the size is 0,
// which will be ignored since nterp is disabled for AOT.
DEFINE_PROTECTED artNterpAsmInstructionStart
    nop
END_FUNCTION artNterpAsmInstructionStart

DEFINE_PROTECTED artNterpAsmInstructionEnd
    nop
END_FUNCTION artNterpAsmInstructionEnd

#endif // __x86_64__

.section .note.GNU-stack,"",@progbits
