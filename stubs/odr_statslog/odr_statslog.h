// Stub odr_statslog.h - not needed for standalone ART build
#ifndef ART_STUB_ODR_STATSLOG_H_
#define ART_STUB_ODR_STATSLOG_H_

#include <string>

namespace odrefresh {

static inline bool UploadStatsIfAvailable(std::string* error) {
  return true;  // No-op: no stats to upload
}

}  // namespace odrefresh

#endif  // ART_STUB_ODR_STATSLOG_H_
