// C++ handler for the replacement art_quick_generic_jni_trampoline.
//
// Called from our minimal assembly trampoline with the ART quick calling
// convention registers already saved to buffers. This function:
// 1. Resolves the native function if needed (JNI dlsym lookup)
// 2. Extracts the method's shorty
// 3. Builds a uint32_t args[] array from the saved registers
// 4. Calls through EnterInterpreterFromInvoke (which uses InterpreterJni
//    for native methods — known to work correctly)
// 5. Returns the result via the result_pair buffer

#include <cstdint>
#include <cstring>
#include <cstdio>

#include "art_method-inl.h"
#include "class_linker.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "interpreter/interpreter.h"
#include "jni/java_vm_ext.h"
#include "jni/jni_env_ext.h"
#include "jvalue.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "thread.h"
#include "stack_reference.h"

extern "C" void artGenericJniTrampolineHandler(
    art::ArtMethod* method,
    art::Thread* self,
    uint64_t* gpr_args,      // Saved x1-x7 (7 registers)
    uint64_t* fpr_args,      // Saved d0-d7 (8 registers)
    uint64_t* result_pair) { // Output: [0]=int_result, [1]=fp_result

  // Initialize results to zero
  result_pair[0] = 0;
  result_pair[1] = 0;

  if (method == nullptr) {
    fprintf(stderr, "[GenericJNI] ERROR: null method\n");
    return;
  }

  // Check if this is actually a native method
  if (!method->IsNative()) {
    fprintf(stderr, "[GenericJNI] ERROR: non-native method %s entered JNI trampoline\n",
            method->PrettyMethod().c_str());
    return;
  }

  // Resolve the native function if it hasn't been registered yet.
  // The JNI entry point may be the dlsym lookup stub, which can't be called
  // from C++ directly. We need to resolve it to the actual native function.
  {
    const void* jni_entry = method->GetEntryPointFromJni();
    const void* dlsym_stub = art::GetJniDlsymLookupStub();
    const void* dlsym_critical_stub = art::GetJniDlsymLookupCriticalStub();

    if (jni_entry == dlsym_stub || jni_entry == dlsym_critical_stub || jni_entry == nullptr) {
      // Need to resolve the native method.
      // Use the ClassLinker's FindCodeForNativeMethod which does JNI name lookup.
      art::JavaVMExt* vm = down_cast<art::JNIEnvExt*>(self->GetJniEnv())->GetVm();
      std::string error_msg;
      const void* native_code = vm->FindCodeForNativeMethod(method, &error_msg, /*can_suspend=*/true);

      if (native_code == nullptr) {
        // Native method not found. Throw UnsatisfiedLinkError.
        fprintf(stderr, "[GenericJNI] UnsatisfiedLinkError: %s (%s)\n",
                method->PrettyMethod().c_str(), error_msg.c_str());
        self->ThrowNewException("Ljava/lang/UnsatisfiedLinkError;", error_msg.c_str());
        return;
      }

      // Register the native code so future calls don't need resolution.
      art::Runtime::Current()->GetClassLinker()->RegisterNative(self, method, native_code);
    }
  }

  // Now the JNI entry point should be the actual native function.
  // Use EnterInterpreterFromInvoke which handles native methods via InterpreterJni.

  // Get the shorty for argument parsing
  const char* shorty_cstr = method->GetShorty();
  if (shorty_cstr == nullptr) {
    fprintf(stderr, "[GenericJNI] ERROR: null shorty for %s\n",
            method->PrettyMethod().c_str());
    return;
  }

  std::string_view shorty(shorty_cstr);
  bool is_static = method->IsStatic();

  // Build args array from the saved registers.
  // ART quick calling convention for ARM64:
  //   x0 = ArtMethod* (not an arg)
  //   x1 = 'this' for instance methods, or first arg for static
  //   x2-x7 = remaining integer/reference args
  //   d0-d7 = float/double args
  //
  // We need to reconstruct the uint32_t args[] that
  // EnterInterpreterFromInvoke expects:
  //   For instance methods: args[0] = this, args[1..] = params
  //   For static methods: args[0..] = params

  // Maximum args: 8 GPR + 8 FPR + generous padding = 32 slots
  uint32_t args[64];
  int arg_idx = 0;
  int gpr_idx = 0;  // index into gpr_args[] (x1-x7)
  int fpr_idx = 0;  // index into fpr_args[] (d0-d7)

  // For instance methods, first arg is 'this' (object reference in x1)
  art::ObjPtr<art::mirror::Object> receiver = nullptr;
  if (!is_static) {
    // 'this' is in x1 (gpr_args[0])
    receiver = reinterpret_cast<art::mirror::Object*>(gpr_args[gpr_idx]);
    args[arg_idx++] = static_cast<uint32_t>(gpr_args[gpr_idx]);
    gpr_idx++;
  }

  // Parse remaining parameters from shorty (skip return type at index 0)
  for (size_t i = 1; i < shorty.size(); i++) {
    switch (shorty[i]) {
      case 'Z':  // boolean
      case 'B':  // byte
      case 'C':  // char
      case 'S':  // short
      case 'I':  // int
        if (gpr_idx < 7) {
          args[arg_idx++] = static_cast<uint32_t>(gpr_args[gpr_idx++]);
        }
        break;

      case 'J':  // long (2 slots)
        if (gpr_idx < 7) {
          uint64_t val = gpr_args[gpr_idx++];
          args[arg_idx++] = static_cast<uint32_t>(val);         // low 32
          args[arg_idx++] = static_cast<uint32_t>(val >> 32);   // high 32
        }
        break;

      case 'F':  // float (1 slot, from FPR)
        if (fpr_idx < 8) {
          uint64_t raw = fpr_args[fpr_idx++];
          uint32_t fval;
          memcpy(&fval, &raw, sizeof(fval));
          args[arg_idx++] = fval;
        }
        break;

      case 'D':  // double (2 slots, from FPR)
        if (fpr_idx < 8) {
          uint64_t raw = fpr_args[fpr_idx++];
          args[arg_idx++] = static_cast<uint32_t>(raw);         // low 32
          args[arg_idx++] = static_cast<uint32_t>(raw >> 32);   // high 32
        }
        break;

      case 'L':  // object reference
        if (gpr_idx < 7) {
          args[arg_idx++] = static_cast<uint32_t>(gpr_args[gpr_idx++]);
        }
        break;

      default:
        fprintf(stderr, "[GenericJNI] WARNING: unknown shorty char '%c'\n", shorty[i]);
        if (gpr_idx < 7) {
          args[arg_idx++] = static_cast<uint32_t>(gpr_args[gpr_idx++]);
        }
        break;
    }
  }

  // Call through EnterInterpreterFromInvoke -> InterpreterJni
  art::JValue result;
  art::ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);

  if (is_static) {
    art::interpreter::EnterInterpreterFromInvoke(
        self, method, nullptr, args, &result, /*stay_in_interpreter=*/true);
  } else {
    art::interpreter::EnterInterpreterFromInvoke(
        self, method, receiver, args + 1, &result, /*stay_in_interpreter=*/true);
  }

  self->PopManagedStackFragment(fragment);

  // Store results
  result_pair[0] = result.GetJ();  // integer result (also works for objects as compressed ref)

  // For float/double results, put the value in the FP slot too
  char ret_type = shorty[0];
  if (ret_type == 'F') {
    float fval = result.GetF();
    uint64_t raw = 0;
    memcpy(&raw, &fval, sizeof(fval));
    result_pair[1] = raw;
  } else if (ret_type == 'D') {
    double dval = result.GetD();
    uint64_t raw;
    memcpy(&raw, &dval, sizeof(raw));
    result_pair[1] = raw;
  } else {
    // For non-FP returns, also put in FP slot (caller uses fmov d0, x0 for FP)
    result_pair[1] = result_pair[0];
  }
}
