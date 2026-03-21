#include "telepath/telemetry/telemetry_sink.h"

#include <memory>

namespace telepath {

class NoOpTelemetrySink : public TelemetrySink {
 public:
  void RecordHit(const BufferTag &) override {}
  void RecordMiss(const BufferTag &) override {}
  void RecordDiskRead(const BufferTag &) override {}
  void RecordDiskWrite(const BufferTag &) override {}
  void RecordEviction(const BufferTag &) override {}
  void RecordDirtyFlush(const BufferTag &) override {}

  TelemetrySnapshot Snapshot() const override { return TelemetrySnapshot{}; }
};

std::shared_ptr<TelemetrySink> MakeNoOpTelemetrySink() {
  return std::make_shared<NoOpTelemetrySink>();
}

}  // namespace telepath
