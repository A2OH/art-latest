// Minimal stubs - no ART headers needed
namespace art {
class Thread;
}
extern "C" {
// art::Thread::InitCpu()
void _ZN3art6Thread7InitCpuEv(void* self) {}
// art::Thread::CleanupCpu()
void _ZN3art6Thread10CleanupCpuEv(void* self) {}
// art::ArtMethod::IsProxyMethod()
bool _ZN3art9ArtMethod13IsProxyMethodEv(void* self) { return false; }
// art::ArtMethod::GetShorty()
const char* _ZN3art9ArtMethod9GetShortyEv(void* self) { return "V"; }
// art::ExecUtils::Exec
int _ZNK3art9ExecUtils4ExecERKNSt6__ndk16vectorINS1_12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEENS6_IS8_EEEEPS8_(void* self, void* args, void* err) { return -1; }
// artCriticalNativeOutArgsSize
int artCriticalNativeOutArgsSize(void* method) { return 0; }
// create_disassembler
void* create_disassembler(int isa, void* output) { return 0; }
}

// android::base stubs
#include <string>
#include <vector>
namespace android { namespace base {
bool ReadFileToString(const std::string& path, std::string* content, bool follow_symlinks) {
    if (content) content->clear();
    return false;
}
}}
