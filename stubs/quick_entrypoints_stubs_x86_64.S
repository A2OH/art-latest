// Stub assembly entry points for Android 15 ART symbols not in Android 11's
// quick_entrypoints_x86_64.S. These are needed to satisfy link-time references
// but dex2oat doesn't call them at runtime (it's an AOT compiler, not a VM).

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
    int3    // Should never be called in dex2oat
    ret
END_FUNCTION art_quick_method_exit_hook

// art_quick_method_entry_hook - instrumentation hook for method entry
DEFINE_FUNCTION art_quick_method_entry_hook
    int3
    ret
END_FUNCTION art_quick_method_entry_hook

// art_quick_compile_optimized, art_quick_osr_stub, art_invoke_obsolete_method_stub
// are already provided by A11's quick_entrypoints_x86_64.S

// Nterp trampolines (protected visibility, used by interpreter)
.macro DEFINE_PROTECTED name
    .type \name, @function
    .protected \name
    .globl \name
    .balign 16
\name:
.endm

DEFINE_PROTECTED NterpGetMethod
    int3
    ret
END_FUNCTION NterpGetMethod

DEFINE_PROTECTED NterpGetStaticField
    int3
    ret
END_FUNCTION NterpGetStaticField

DEFINE_PROTECTED NterpGetInstanceFieldOffset
    int3
    ret
END_FUNCTION NterpGetInstanceFieldOffset

#endif // __x86_64__

.section .note.GNU-stack,"",@progbits
