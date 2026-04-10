// Stubs for arch-specific fault handler virtual methods (Android 15).
// dex2oat never triggers these handlers.

#include "fault_handler.h"
#include <signal.h>

namespace art {

bool NullPointerHandler::Action(int, siginfo_t*, void*) {
  return false;
}

bool SuspensionHandler::Action(int, siginfo_t*, void*) {
  return false;
}

bool StackOverflowHandler::Action(int, siginfo_t*, void*) {
  return false;
}

// A15 replaced GetMethodAndReturnPcAndSp with GetFaultPc/GetFaultSp
uintptr_t FaultManager::GetFaultPc(siginfo_t*, void*) {
  return 0;
}

uintptr_t FaultManager::GetFaultSp(void*) {
  return 0;
}

}  // namespace art

// Stub membarrier for kernel 4.9 compatibility
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

extern "C" int membarrier(int cmd, unsigned int flags, int cpu_id) {
    // Return success for all membarrier commands
    // On kernel 4.9, the real syscall fails — this stub prevents crashes
    return 0;
}
