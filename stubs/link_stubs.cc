// link_stubs.cc - Stub implementations for symbols that dex2oat/dalvikvm
// reference but don't actually call during AOT compilation.
// Android 15 ART version.

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdarg.h>
#include <dlfcn.h>

extern "C" {

// === liblog ===
// Minimal implementation that routes all log output to stderr.

struct __android_log_message {
    size_t struct_size;
    int32_t buffer_id;
    int32_t priority;
    const char* tag;
    const char* file;
    uint32_t line;
    const char* message;
};

typedef void (*__android_logger_function)(const struct __android_log_message* log_message);
typedef void (*__android_aborter_function)(const char* abort_message);

static __android_logger_function g_logger = nullptr;
static __android_aborter_function g_aborter = nullptr;
static int g_min_priority = 0;
static const char* g_default_tag = "dex2oat";

static void default_stderr_logger(const struct __android_log_message* msg) {
    fprintf(stderr, "%s: %s\n", msg->tag ? msg->tag : g_default_tag, msg->message ? msg->message : "");
    fflush(stderr);
}

static void default_aborter(const char* msg) {
    fprintf(stderr, "ABORT: %s\n", msg);
    fflush(stderr);
    abort();
}

int __android_log_buf_print(int, int priority, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", tag ? tag : g_default_tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
    return 0;
}
int __android_log_print(int priority, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", tag ? tag : g_default_tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
    return 0;
}
int __android_log_is_loggable(int priority, const char*, int) { return priority >= g_min_priority; }
void __android_log_set_logger(__android_logger_function logger) { g_logger = logger; }
void __android_log_write_log_message(struct __android_log_message* msg) {
    if (g_logger) {
        g_logger(msg);
    } else {
        default_stderr_logger(msg);
    }
}
void __android_log_logd_logger(const struct __android_log_message* msg) {
    default_stderr_logger(msg);
}
void __android_log_stderr_logger(const struct __android_log_message* msg) {
    default_stderr_logger(msg);
}
void __android_log_set_aborter(__android_aborter_function aborter) { g_aborter = aborter; }
void __android_log_call_aborter(const char* msg) {
    if (g_aborter) g_aborter(msg); else default_aborter(msg);
}
void __android_log_default_aborter(const char* msg) { default_aborter(msg); }
int __android_log_set_minimum_priority(int p) { int old = g_min_priority; g_min_priority = p; return old; }
int __android_log_get_minimum_priority() { return g_min_priority; }
void __android_log_set_default_tag(const char* tag) { if (tag) g_default_tag = tag; }
int __android_log_write(int priority, const char* tag, const char* text) {
    fprintf(stderr, "%s: %s\n", tag ? tag : "art", text ? text : "");
    fflush(stderr);
    return 0;
}
void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    va_list ap;
    if (fmt) {
        va_start(ap, fmt);
        fprintf(stderr, "%s: ASSERT FAILED (%s): ", tag ? tag : "art", cond ? cond : "");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    } else {
        fprintf(stderr, "%s: ASSERT FAILED: %s\n", tag ? tag : "art", cond ? cond : "");
    }
    fflush(stderr);
    abort();
}

// === LZ4 ===
int LZ4_compressBound(int inputSize) { return inputSize + inputSize / 255 + 16; }
int LZ4_compress_default(const char* src, char* dst, int srcSize, int dstCapacity) {
    if (srcSize > dstCapacity) return 0;
    memcpy(dst, src, srcSize);
    return srcSize;
}
int LZ4_compress_HC(const char* src, char* dst, int srcSize, int dstCapacity, int) {
    return LZ4_compress_default(src, dst, srcSize, dstCapacity);
}
int LZ4_decompress_safe(const char* src, char* dst, int compressedSize, int dstCapacity) {
    if (compressedSize > dstCapacity) return -1;
    memcpy(dst, src, compressedSize);
    return compressedSize;
}

// === ART palette (expanded for Android 15) ===
// palette_status_t is an enum; PALETTE_STATUS_OK = 0, PALETTE_STATUS_NOT_SUPPORTED = 4
int PaletteTraceEnabled(bool* enabled) { if (enabled) *enabled = false; return 0; }
int PaletteTraceBegin(const char*) { return 0; }
int PaletteTraceEnd() { return 0; }
int PaletteTraceIntegerValue(const char*, int) { return 0; }
int PaletteSchedGetPriority(int, int* p) { if (p) *p = 0; return 0; }
int PaletteSchedSetPriority(int, int) { return 0; }
int PaletteWriteCrashThreadStacks(const char*, unsigned long) { return 0; }
int PaletteAshmemCreateRegion(const char*, unsigned long, int*) { return 4; /* NOT_SUPPORTED */ }
int PaletteAshmemSetProtRegion(int, int) { return 4; }
int PaletteCreateOdrefreshStagingDirectory(const char**) { return 4; }
int PaletteShouldReportDex2oatCompilation(bool* v) { if (v) *v = false; return 0; }
int PaletteNotifyStartDex2oatCompilation(int, int, int, int) { return 0; }
int PaletteNotifyEndDex2oatCompilation(int, int, int, int) { return 0; }
int PaletteNotifyDexFileLoaded(const char*) { return 0; }
int PaletteNotifyOatFileLoaded(const char*) { return 0; }
int PaletteShouldReportJniInvocations(bool* v) { if (v) *v = false; return 0; }
int PaletteNotifyBeginJniInvocation(void*) { return 0; }
int PaletteNotifyEndJniInvocation(void*) { return 0; }
int PaletteReportLockContention(void*, int, const char*, int, const char*, const char*, int, const char*, const char*, const char*) { return 0; }
int PaletteSetTaskProfiles(int, const char* const*, unsigned long) { return 4; }
int PaletteDebugStoreGetString(char* result, unsigned long max_size) {
    if (!result || max_size == 0) return 3; /* INVALID_ARGUMENT */
    result[0] = '\0';
    return 0;
}

// === Native bridge (not used in dex2oat) ===
int InitializeNativeBridge(void*, const char*) { return 0; }
int LoadNativeBridge(const char*, void*) { return 0; }
int NativeBridgeInitialized() { return 0; }
int NativeBridgeGetVersion() { return 0; }
void* NativeBridgeGetTrampoline(void*, const char*, const char*, int) { return nullptr; }
void* NativeBridgeGetSignalHandler(int) { return nullptr; }
void PreInitializeNativeBridge(const char*, const char*) {}
void PreZygoteForkNativeBridge() {}
void UnloadNativeBridge() {}

// === Native loader (not used in dex2oat) ===
int InitializeNativeLoader() { return 0; }
int ResetNativeLoader() { return 0; }
void* OpenNativeLibrary(void* env, int target_sdk, const char* path, void* class_loader,
                        const char* caller_location, void* library_path,
                        bool* needs_native_bridge, char** error_msg) {
    if (needs_native_bridge) *needs_native_bridge = false;
    if (error_msg) *error_msg = NULL;
    void* handle = dlopen(path, RTLD_NOW);
    if (!handle && error_msg) {
        const char* err = dlerror();
        if (err) *error_msg = strdup(err);
    }
    return handle;
}
int CloseNativeLibrary(void*, void*, void*) { return 0; }
void NativeLoaderFreeErrorMessage(char*) {}

// === LZMA (used for compressed OAT) ===
void Lzma2EncProps_Init(void*) {}
void Lzma2EncProps_Normalize(void*) {}
int Xz_Encode(void*, void*, void*, void*) { return -1; }
void XzProps_Init(void*) {}
void XzUnpacker_Construct(void*, void*) {}
void XzUnpacker_Free(void*) {}
int XzUnpacker_Code(void*, void*, void*, void*, void*, int, void*) { return -1; }
int XzUnpacker_IsStreamWasFinished(void*) { return 1; }
void CrcGenerateTable() {}
void Crc64GenerateTable() {}

// === SHA1 ===
void SHA1_Init(void*) {}
void SHA1_Update(void* ctx, const void* data, unsigned long len) {}
void SHA1_Final(unsigned char* md, void* ctx) { if (md) memset(md, 0, 20); }

// === Remaining stub entrypoints ===
#define STUB_ENTRYPOINT(name) void name() { fprintf(stderr, "FATAL: stub entrypoint " #name " called\n"); abort(); }

// Interpreter stubs (mterp/nterp not in x86_64 quick_entrypoints)
STUB_ENTRYPOINT(ExecuteMterpImpl)
STUB_ENTRYPOINT(ExecuteNterpImpl)
STUB_ENTRYPOINT(ExecuteNterpWithClinitImpl)

// artMterpAsmInstructionStart/End must be spaced exactly 256*128 = 32768 bytes apart
__attribute__((aligned(128))) char artMterpAsmInstructionStart[32768] = {};
char artMterpAsmInstructionEnd[1] = {};

// artNterpAsmInstructionStart/End
__attribute__((aligned(128))) char artNterpAsmInstructionStart[32768] = {};
char artNterpAsmInstructionEnd[1] = {};

// art_quick_invoke_stub_internal is ARM-only
STUB_ENTRYPOINT(art_quick_invoke_stub_internal)

// A15 JNI lock/unlock entry points (not in A11 assembly)
void art_jni_lock_object(void*) {}
void art_jni_lock_object_no_inline(void*) {}
void art_jni_unlock_object(void*) {}
void art_jni_unlock_object_no_inline(void*) {}

// A15 JNI entry point stubs
void art_jni_monitored_method_start(void*) {}
void art_jni_monitored_method_end(void*) {}
void art_jni_method_start(void*) {}
void art_jni_method_end(void*) {}
void art_jni_method_entry_hook(void*) {}
void art_jni_read_barrier(void*) { }

// A15-only art_quick_* entry points not in A11 assembly
STUB_ENTRYPOINT(art_quick_invoke_polymorphic_with_hidden_receiver)

// Most art_quick_* entry points already exist in A11 quick_entrypoints_x86_64.S.

// NativeBridge stub
void* NativeBridgeGetTrampoline2(void*, const char*, const char*, uint32_t) { return nullptr; }

}  // extern "C"

// === C++ stubs in art namespace ===
#include <ostream>
#include <vector>

// === Nterp C++ stubs (art::interpreter namespace) ===
// These are in nterp.cc which is excluded from the build (requires full mterp assembly).
namespace art {
template <typename T> class ArrayRef {
  const T* data_ = nullptr;
  size_t size_ = 0;
public:
  ArrayRef() = default;
  ArrayRef(const T* d, size_t s) : data_(d), size_(s) {}
};
namespace interpreter {
bool IsNterpSupported() { return false; }
const void* GetNterpEntryPoint() { return nullptr; }
const void* GetNterpWithClinitEntryPoint() { return nullptr; }
bool CanRuntimeUseNterp() { return false; }
ArrayRef<const uint8_t> NterpWithClinitImpl() { return ArrayRef<const uint8_t>(); }
ArrayRef<const uint8_t> NterpImpl() { return ArrayRef<const uint8_t>(); }
void CheckNterpAsmConstants() {}
}  // namespace interpreter
}  // namespace art

// === BacktraceMap ===
class BacktraceMap {
public:
    static BacktraceMap* Create(int, bool);
};
BacktraceMap* BacktraceMap::Create(int, bool) { return nullptr; }

namespace art {

// DumpNativeStack - needs libbacktrace/libunwindstack
class ArtMethod;
void DumpNativeStack(std::ostream&, int, BacktraceMap*, const char*, ArtMethod*, void*, bool) {}
// Android 15 version uses unwindstack::AndroidLocalUnwinder instead of BacktraceMap
}  // namespace art (temporarily close)
namespace unwindstack { class AndroidLocalUnwinder; }
namespace art {
void DumpNativeStack(std::ostream&, ::unwindstack::AndroidLocalUnwinder&, int, const char*, ArtMethod*, void*, bool) {}
// A15 simplified overload (no unwinder parameter)
void DumpNativeStack(std::ostream&, int, const char*, ArtMethod*, void*, bool) {}

// BacktraceCollector - needs libunwindstack
class BacktraceCollector {
public:
  void Collect();
};
void BacktraceCollector::Collect() {}

// SafeCopy - needs sys/ucontext
size_t SafeCopy(void* dst, const void* src, size_t len) {
    memcpy(dst, src, len);
    return len;
}

// hprof - not needed for dex2oat
namespace hprof {
  void DumpHeap(const char*, int, bool) {}
}

}  // namespace art

// =============================================================================
// === operator<< stubs for ART enums ===
// =============================================================================

#include <atomic>

namespace art {

// Forward declarations for enum types
enum class InstructionSet;
enum class InvokeType : uint32_t;
enum class LayoutType : uint8_t;
enum class ClassStatus : uint8_t;
enum class ThreadState : uint16_t;
enum class JdwpProvider;
enum class OatClassType : uint8_t;
enum class SuspendReason : uint32_t;
enum class IndirectRefKind;
enum class ReflectionSourceType;
enum VRegKind : int;
enum RootType : int;
enum class JniIdType;
enum class CompilationKind;  // New in A15

std::ostream& operator<<(std::ostream& os, InstructionSet rhs) {
  return os << "InstructionSet(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, InvokeType rhs) {
  return os << "InvokeType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, LayoutType rhs) {
  return os << "LayoutType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, ClassStatus rhs) {
  return os << "ClassStatus(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, ThreadState rhs) {
  return os << "ThreadState(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, JdwpProvider rhs) {
  return os << "JdwpProvider(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, OatClassType rhs) {
  return os << "OatClassType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, SuspendReason rhs) {
  return os << "SuspendReason(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, IndirectRefKind rhs) {
  return os << "IndirectRefKind(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, ReflectionSourceType rhs) {
  return os << "ReflectionSourceType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, VRegKind rhs) {
  return os << "VRegKind(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, RootType rhs) {
  return os << "RootType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, JniIdType rhs) {
  return os << "JniIdType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, CompilationKind rhs) {
  return os << "CompilationKind(" << static_cast<int>(rhs) << ")";
}

// MethodCompilationStat - in compiler/optimizing
enum class MethodCompilationStat;
std::ostream& operator<<(std::ostream& os, MethodCompilationStat rhs) {
  return os << "MethodCompilationStat(" << static_cast<int>(rhs) << ")";
}

// WriteBarrierKind - new in Android 15
enum class WriteBarrierKind;
std::ostream& operator<<(std::ostream& os, WriteBarrierKind rhs) {
  return os << "WriteBarrierKind(" << static_cast<int>(rhs) << ")";
}

// MemBarrierKind - used in code generators
enum class MemBarrierKind;
std::ostream& operator<<(std::ostream& os, MemBarrierKind rhs) {
  return os << "MemBarrierKind(" << static_cast<int>(rhs) << ")";
}

// MethodLoadKind
enum class MethodLoadKind;
std::ostream& operator<<(std::ostream& os, MethodLoadKind rhs) {
  return os << "MethodLoadKind(" << static_cast<int>(rhs) << ")";
}

// HLoadClass::LoadKind
class HLoadClass {
public:
  enum class LoadKind;
};
std::ostream& operator<<(std::ostream& os, HLoadClass::LoadKind rhs) {
  return os << "LoadKind(" << static_cast<int>(rhs) << ")";
}

// DataType::Type - provided by compiler/optimizing/data_type.o

// LockLevel
enum class LockLevel;
std::ostream& operator<<(std::ostream& os, LockLevel rhs) {
  return os << "LockLevel(" << static_cast<int>(rhs) << ")";
}

// GraphAnalysisResult - compiler
enum class GraphAnalysisResult;
std::ostream& operator<<(std::ostream& os, GraphAnalysisResult rhs) {
  return os << "GraphAnalysisResult(" << static_cast<int>(rhs) << ")";
}

// HLoadString::LoadKind
class HLoadString {
public:
  enum class LoadKind;
};
std::ostream& operator<<(std::ostream& os, HLoadString::LoadKind rhs) {
  return os << "LoadKind(" << static_cast<int>(rhs) << ")";
}

// --- ImageHeader nested enums ---
class ImageHeader {
public:
  enum ImageMethod : int;
  enum ImageSections : int;
  enum StorageMode : int;
};
std::ostream& operator<<(std::ostream& os, ImageHeader::ImageMethod rhs) {
  return os << "ImageMethod(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, ImageHeader::ImageSections rhs) {
  return os << "ImageSections(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, ImageHeader::StorageMode rhs) {
  return os << "StorageMode(" << static_cast<int>(rhs) << ")";
}

// --- EncodedArrayValueIterator nested enum ---
class EncodedArrayValueIterator {
public:
  enum class ValueType : uint8_t;
};
std::ostream& operator<<(std::ostream& os, EncodedArrayValueIterator::ValueType rhs) {
  return os << "ValueType(" << static_cast<int>(rhs) << ")";
}

// --- DexLayoutSections nested enum ---
class DexLayoutSections {
public:
  enum class SectionType : uint8_t;
};
std::ostream& operator<<(std::ostream& os, DexLayoutSections::SectionType rhs) {
  return os << "SectionType(" << static_cast<int>(rhs) << ")";
}

// --- Location nested enums ---
class Location {
public:
  enum class Kind : uint8_t;
  enum class Policy : uint8_t;
};
std::ostream& operator<<(std::ostream& os, Location::Kind rhs) {
  return os << "Location::Kind(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, Location::Policy rhs) {
  return os << "Location::Policy(" << static_cast<int>(rhs) << ")";
}

// --- LockWord nested enum ---
class LockWord {
public:
  enum LockState : int;
};
std::ostream& operator<<(std::ostream& os, LockWord::LockState rhs) {
  return os << "LockState(" << static_cast<int>(rhs) << ")";
}

// XzCompress / XzDecompress now provided by libelffile/elf/xz_utils.o (Android 15)

}  // namespace art

// --- art::gc:: namespace enums ---
namespace art {
namespace gc {

enum class AllocatorType;
enum class CollectorType;
enum class WeakRootState;

std::ostream& operator<<(std::ostream& os, AllocatorType rhs) {
  return os << "AllocatorType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, CollectorType rhs) {
  return os << "CollectorType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, WeakRootState rhs) {
  return os << "WeakRootState(" << static_cast<int>(rhs) << ")";
}

namespace space {

enum class GcRetentionPolicy;
enum class SpaceType;

std::ostream& operator<<(std::ostream& os, GcRetentionPolicy rhs) {
  return os << "GcRetentionPolicy(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, SpaceType rhs) {
  return os << "SpaceType(" << static_cast<int>(rhs) << ")";
}

class RegionSpace {
public:
  enum class RegionType : uint8_t;
  enum class RegionState : uint8_t;
};
std::ostream& operator<<(std::ostream& os, RegionSpace::RegionType rhs) {
  return os << "RegionType(" << static_cast<int>(rhs) << ")";
}
// Android 15 ART passes RegionState by value (not const ref)
std::ostream& operator<<(std::ostream& os, RegionSpace::RegionState rhs) {
  return os << "RegionState(" << static_cast<int>(rhs) << ")";
}

}  // namespace space

namespace allocator {

class RosAlloc {
public:
  enum PageMapKind : uint8_t;
};
std::ostream& operator<<(std::ostream& os, RosAlloc::PageMapKind rhs) {
  return os << "PageMapKind(" << static_cast<int>(rhs) << ")";
}

}  // namespace allocator

namespace collector {

enum GcType : int;
std::ostream& operator<<(std::ostream& os, GcType rhs) {
  return os << "GcType(" << static_cast<int>(rhs) << ")";
}

}  // namespace collector

}  // namespace gc
}  // namespace art

// --- art::instrumentation:: ---
namespace art {
namespace instrumentation {

class Instrumentation {
public:
  enum InstrumentationEvent : uint32_t;
  enum class InstrumentationLevel;
};
std::ostream& operator<<(std::ostream& os, Instrumentation::InstrumentationEvent rhs) {
  return os << "InstrumentationEvent(0x" << std::hex << static_cast<unsigned>(rhs) << std::dec << ")";
}
std::ostream& operator<<(std::ostream& os, Instrumentation::InstrumentationLevel rhs) {
  return os << "InstrumentationLevel(" << static_cast<int>(rhs) << ")";
}

}  // namespace instrumentation
}  // namespace art

// --- art::linker:: ---
namespace art {
namespace linker {

class LinkerPatch {
public:
  enum class Type : uint8_t;
};
std::ostream& operator<<(std::ostream& os, LinkerPatch::Type rhs) {
  return os << "LinkerPatch::Type(" << static_cast<int>(rhs) << ")";
}

}  // namespace linker
}  // namespace art

// --- art::verifier:: ---
namespace art {
namespace verifier {

enum class MethodType : uint32_t;
enum class VerifyError : uint32_t;

std::ostream& operator<<(std::ostream& os, MethodType rhs) {
  return os << "MethodType(" << static_cast<int>(rhs) << ")";
}
std::ostream& operator<<(std::ostream& os, VerifyError rhs) {
  return os << "VerifyError(0x" << std::hex << static_cast<unsigned>(rhs) << std::dec << ")";
}

}  // namespace verifier
}  // namespace art

// =============================================================================
// === DexCache 128-bit atomic operations ===
// =============================================================================

namespace art {
namespace mirror {

class DexCache {
public:
  template <typename IntType>
  struct __attribute__((packed)) ConversionPair {
    IntType first;
    IntType second;
  };
  using ConversionPair64 = ConversionPair<uint64_t>;

  static ConversionPair64 AtomicLoadRelaxed16B(std::atomic<ConversionPair64>* target);
  static void AtomicStoreRelease16B(std::atomic<ConversionPair64>* target, ConversionPair64 value);
};

DexCache::ConversionPair64 DexCache::AtomicLoadRelaxed16B(
    std::atomic<ConversionPair64>* target) {
  uint64_t lo, hi;
  __asm__ __volatile__(
      "xorq %%rax, %%rax\n\t"
      "xorq %%rdx, %%rdx\n\t"
      "xorq %%rbx, %%rbx\n\t"
      "xorq %%rcx, %%rcx\n\t"
      "lock cmpxchg16b (%[ptr])\n\t"
      : "=a"(lo), "=d"(hi)
      : [ptr] "r"(target)
      : "rbx", "rcx", "cc", "memory");
  ConversionPair64 result;
  result.first = lo;
  result.second = hi;
  return result;
}

void DexCache::AtomicStoreRelease16B(
    std::atomic<ConversionPair64>* target, ConversionPair64 value) {
  uint64_t new_lo = value.first;
  uint64_t new_hi = value.second;
  uint64_t old_lo, old_hi;
  __asm__ __volatile__(
      "xorq %%rax, %%rax\n\t"
      "xorq %%rdx, %%rdx\n\t"
      "xorq %%rbx, %%rbx\n\t"
      "xorq %%rcx, %%rcx\n\t"
      "lock cmpxchg16b (%[ptr])\n\t"
      : "=a"(old_lo), "=d"(old_hi)
      : [ptr] "r"(target)
      : "rbx", "rcx", "cc", "memory");
  bool success;
  do {
    __asm__ __volatile__(
        "lock cmpxchg16b (%[ptr])\n\t"
        "setz %[success]\n\t"
        : "=a"(old_lo), "=d"(old_hi), [success] "=r"(success)
        : "a"(old_lo), "d"(old_hi), "b"(new_lo), "c"(new_hi), [ptr] "r"(target)
        : "cc", "memory");
  } while (!success);
}

}  // namespace mirror
}  // namespace art

// XzCompress/XzDecompress by-value overloads now provided by xz_utils.o
