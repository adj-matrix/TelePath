#include "telepath/telemetry/telemetry_sink.h"

#include <memory>

namespace telepath {

class NoOpTelemetrySink : public TelemetrySink {
 public:
  NoOpTelemetrySink() = default;

 protected:
  void DoRecordHit(const BufferTag &) override {}
  void DoRecordMiss(const BufferTag &) override {}
  void DoRecordDiskRead(const BufferTag &) override {}
  void DoRecordDiskWrite(const BufferTag &) override {}
  void DoRecordEviction(const BufferTag &) override {}
  void DoRecordDirtyFlush(const BufferTag &) override {}

  TelemetrySnapshot DoSnapshot() const override { return TelemetrySnapshot{}; }
};

std::shared_ptr<TelemetrySink> MakeNoOpTelemetrySink() {
  return std::make_shared<NoOpTelemetrySink>();
}

}  // namespace telepath
