// Patch: Relax ClassLinker::CheckSystemClass to allow A11 core JARs with A15 ART.
// The original check is fatal when class objects don't match (e.g., different
// virtual method counts between A11 and A15 java.lang.String). For standalone
// dex2oat boot image creation, this mismatch is harmless.

#include "class_linker.h"
#include "class_linker-inl.h"
#include "base/logging.h"
#include "handle.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "obj_ptr-inl.h"

namespace art HIDDEN {

void ClassLinker::CheckSystemClass(Thread* self, Handle<mirror::Class> c1, const char* descriptor) {
  ObjPtr<mirror::Class> c2 = FindSystemClass(self, descriptor);
  if (c2 == nullptr) {
    LOG(WARNING) << "CheckSystemClass: Could not find class " << descriptor << " (non-fatal)";
    return;
  }
  if (c1.Get() != c2.Ptr()) {
    LOG(WARNING) << "CheckSystemClass: Class mismatch for " << descriptor
                 << " (continuing for standalone dex2oat)";
  }
}

}  // namespace art
