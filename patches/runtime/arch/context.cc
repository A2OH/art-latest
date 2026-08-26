/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "arch/context-inl.h"

#include <cstdio>
#include <cstdlib>

#include "thread.h"
#include "art_method-inl.h"
#include "arch/arm64/context_arm64.h"

namespace art HIDDEN {

Context* Context::Create() {
  Context* ctx = new RuntimeContextType;
  /* PF-arch-021: diagnose where bad-vtable contexts come from. */
  void* vt = *reinterpret_cast<void**>(ctx);
  if (vt == nullptr) {
    static thread_local int diag_count = 0;
    if (diag_count < 5) {
      diag_count++;
      fprintf(stderr, "[PF-arch-021] Context::Create returned NULL vtable! ctx=%p\n", ctx);
      fflush(stderr);
    }
  }
  return ctx;
}

/* PF-arch-016: Defensive null-guard on Context vtable.
 * Observed SIGBUS pc=0x87a2b0 (ldr x8, [x8, #128]) where x8=0 → vtable pointer
 * at offset 0 of Context was NULL. Triggered during MainActivity ctor exception
 * unwind. Log + return early instead of dereferencing NULL vtable. */
extern "C" void artContextCopyForLongJump(Context* context, uintptr_t* gprs, uintptr_t* fprs) {
  if (context == nullptr) {
    fprintf(stderr, "[PF-arch-016] artContextCopyForLongJump: NULL context, aborting\n");
    fflush(stderr);
    abort();
  }
  void* vtable_ptr = *reinterpret_cast<void**>(context);
  if (vtable_ptr == nullptr) {
    fprintf(stderr,
            "[PF-arch-016] NULL vtable context=%p — exception undeliverable\n",
            static_cast<void*>(context));
    Thread* self = Thread::Current();
    if (self != nullptr) {
      mirror::Throwable* ex = self->GetException();
      if (ex != nullptr) {
        std::string ex_class = ex->GetClass()->PrettyDescriptor();
        fprintf(stderr, "[PF-arch-016]   exception_class=%s\n", ex_class.c_str());
        std::string ex_msg = ex->Dump();
        fprintf(stderr, "[PF-arch-016]   exception_dump=%s\n", ex_msg.c_str());
      }
    }
    fflush(stderr);
    /* Exception delivery via long-jump fundamentally unavailable in our
     * standalone build. Exit cleanly so post-mortem shows the actual cause
     * (the exception_dump above) instead of a confusing trampoline SIGBUS. */
    _exit(134);
  }
  context->CopyContextTo(gprs, fprs);
  // Once the context has been copied, it is no longer needed.
  // The context pointer is passed via hand-written assembly stubs, otherwise we'd take the
  // context argument as a `std::unique_ptr<>` to indicate the ownership handover.
  delete context;
}

// Force vtable emission for Context base class.
Context::~Context() {}

}  // namespace art
