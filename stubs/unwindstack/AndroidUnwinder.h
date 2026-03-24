// Stub unwindstack/AndroidUnwinder.h - not available in standalone build
#ifndef ART_STUB_UNWINDSTACK_ANDROID_UNWINDER_H_
#define ART_STUB_UNWINDSTACK_ANDROID_UNWINDER_H_

#include <string>
#include <vector>

namespace unwindstack {

struct FrameData {
  uint64_t pc;
  uint64_t sp;
  std::string function_name;
  uint64_t function_offset;
};

class AndroidUnwinder {
public:
  struct UnwindResult {
    bool ok = false;
    std::vector<FrameData> frames;
  };
  AndroidUnwinder() {}
  virtual ~AndroidUnwinder() {}
};

class AndroidLocalUnwinder : public AndroidUnwinder {
public:
  AndroidLocalUnwinder() {}
  AndroidLocalUnwinder(const std::vector<std::string>&) {}
  AndroidLocalUnwinder(const std::vector<std::string>&, const std::vector<std::string>&) {}
};

}  // namespace unwindstack

#endif  // ART_STUB_UNWINDSTACK_ANDROID_UNWINDER_H_
