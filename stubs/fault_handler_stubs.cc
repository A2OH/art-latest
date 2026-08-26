// Stubs for arch-specific fault handler virtual methods (Android 15).
// dex2oat never triggers these handlers.

// 2026-07-09: the arch-specific fault-handler symbols (NullPointerHandler::Action,
// SuspensionHandler::Action, StackOverflowHandler::Action, FaultManager::GetFaultPc/
// GetFaultSp) are now provided by the REAL patches/runtime/arch/arm64/fault_handler_arm64.cc
// (compiled back in). These no-op stubs disabled ALL implicit-null-check recovery in
// the runtime, making every framework null deref a fatal exit(1) (the ~18s parent
// death). Removed here so the real handler is linked instead. dex2oat, which never
// triggers these, uses its own build that keeps them stubbed.

// Stub membarrier for kernel 4.9 compatibility
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

extern "C" int membarrier(int cmd, unsigned int flags, int cpu_id) {
    // Return success for all membarrier commands
    // On kernel 4.9, the real syscall fails — this stub prevents crashes
    return 0;
}
