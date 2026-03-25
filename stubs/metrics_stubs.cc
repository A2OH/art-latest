// Stub implementations for art::metrics classes that depend on tinyxml2 features
// not available in the Android 11 tinyxml2 library (InsertNewChildElement).
// ArtMetrics itself is now compiled from metrics_common.cc (the real source).

#include <ostream>
#include <vector>
#include <memory>
#include <string>

// Forward declarations matching the ART metrics API
namespace android { namespace base { enum LogSeverity : unsigned int; } }

namespace art HIDDEN {
namespace metrics {

class MetricsBackend;
class MetricsFormatter {
 public:
  virtual ~MetricsFormatter() = default;
};

// XmlFormatter uses InsertNewChildElement which doesn't exist in A11 tinyxml2.
// Since dex2oat never uses XML metrics output, we just stub the destructor.
class XmlFormatter : public MetricsFormatter {
 public:
  ~XmlFormatter() override;
};
XmlFormatter::~XmlFormatter() = default;

class TextFormatter : public MetricsFormatter {
 public:
  ~TextFormatter() override;
};
TextFormatter::~TextFormatter() = default;

class LogBackend {
 public:
  LogBackend(std::unique_ptr<MetricsFormatter>, android::base::LogSeverity);
};
LogBackend::LogBackend(std::unique_ptr<MetricsFormatter>, android::base::LogSeverity) {}

class FileBackend {
 public:
  FileBackend(std::unique_ptr<MetricsFormatter>, const std::string&);
};
FileBackend::FileBackend(std::unique_ptr<MetricsFormatter>, const std::string&) {}

}  // namespace metrics
}  // namespace art
