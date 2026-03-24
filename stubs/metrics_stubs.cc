// Stub implementations for art::metrics classes.
// metrics_common.cc requires a newer tinyxml2 (with InsertNewChildElement)
// than what AOSP Android 11 provides. These stubs satisfy link-time references.

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

class ArtMetrics {
 public:
  ArtMetrics();
  void DumpForSigQuit(std::ostream&);
  void ReportAllMetricsAndResetValueMetrics(const std::vector<MetricsBackend*>&);
  void Reset();
};
ArtMetrics::ArtMetrics() {}
void ArtMetrics::DumpForSigQuit(std::ostream&) {}
void ArtMetrics::ReportAllMetricsAndResetValueMetrics(const std::vector<MetricsBackend*>&) {}
void ArtMetrics::Reset() {}

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

struct SessionData {
  static SessionData CreateDefault();
};
SessionData SessionData::CreateDefault() { return SessionData{}; }

}  // namespace metrics
}  // namespace art
