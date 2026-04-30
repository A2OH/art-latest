#include <cstddef>
#include <cstdint>
#include <sys/types.h>

// Minimal stubs - no ART headers needed
namespace art {
class Thread;
}
extern "C" {
char _DYNAMIC[1] = {};

// art::Thread::InitCpu()
void __attribute__((weak)) _ZN3art6Thread7InitCpuEv(void* self) {}
// art::Thread::CleanupCpu()
void __attribute__((weak)) _ZN3art6Thread10CleanupCpuEv(void* self) {}
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
// libwebp is not linked in the current portable static runtime. Returning
// null lets the Java image path fall back without leaving a strong symbol.
uint8_t* WebPDecodeRGBA(const uint8_t* data, size_t data_size, int* w, int* h) { return nullptr; }
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

// ExecUtils vtable stub
namespace art {
class ExecUtils {
public:
  virtual ~ExecUtils() {}
  virtual int Exec(const void* args, void* err) const { return -1; }
};
// Force vtable emission
static ExecUtils __exec_utils_vtable_anchor;
}

// android::base file descriptor stubs
namespace android { namespace base {
int SendFileDescriptorVector(int fd, const void* data, unsigned long len, const void* fds) { return -1; }
int ReceiveFileDescriptorVector(int fd, void* data, unsigned long len, unsigned long maxfds, void* out) { return -1; }
}}

extern "C" ssize_t _ZN7android4base24SendFileDescriptorVectorENS0_11borrowed_fdEPKvmRKNSt6__ndk16vectorIiNS4_9allocatorIiEEEE(
    void*, const void*, unsigned long, const void*) {
  return -1;
}

extern "C" ssize_t _ZN7android4base27ReceiveFileDescriptorVectorENS0_11borrowed_fdEPvmmPNSt6__ndk16vectorINS0_14unique_fd_implINS0_13DefaultCloserEEENS3_9allocatorIS7_EEEE(
    void*, void*, unsigned long, unsigned long, void*) {
  return -1;
}

extern "C" ssize_t _ZN7android4base24SendFileDescriptorVectorENS0_11borrowed_fdEPKvmRKNSt3__h6vectorIiNS4_9allocatorIiEEEE(
    void*, const void*, unsigned long, const void*) {
  return -1;
}

extern "C" ssize_t _ZN7android4base27ReceiveFileDescriptorVectorENS0_11borrowed_fdEPvmmPNSt3__h6vectorINS0_14unique_fd_implINS0_13DefaultCloserEEENS3_9allocatorIS7_EEEE(
    void*, void*, unsigned long, unsigned long, void*) {
  return -1;
}

// Log error write
extern "C" int __android_log_error_write(int tag, const char* subTag, int uid, const char* data, int dataLen) { return 0; }
