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
