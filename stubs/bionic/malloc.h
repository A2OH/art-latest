// Stub bionic/malloc.h for standalone static build.
// M_MEMTAG_STACK_IS_ON is not defined, so the MTE code path is disabled.
#pragma once

// android_mallopt is a bionic extension not available in NDK headers.
// We don't need MTE (Memory Tagging Extension) support for standalone dalvikvm.
// By not defining M_MEMTAG_STACK_IS_ON, the guarded code in context_arm64.cc is skipped.
