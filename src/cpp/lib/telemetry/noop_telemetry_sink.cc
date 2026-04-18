#include "telepath/telemetry/telemetry_sink.h"

#include <memory>

namespace telepath {

class NoOpTelemetrySink final : public TelemetrySink {
 public:
  NoOpTelemetrySink() = default;

 protected:
  void DoRecordHit(const BufferTag &) override {}
  void DoRecordMiss(const BufferTag &) override {}
  void DoRecordDiskRead(const BufferTag &) override {}
  void DoRecordDiskWrite(const BufferTag &) override {}
  void DoRecordEviction(const BufferTag &) override {}
  void DoRecordDirtyFlush(const BufferTag &) override {}

  auto DoSnapshot() const -> TelemetrySnapshot override {
    return TelemetrySnapshot{};
  }
};

auto MakeNoOpTelemetrySink() -> std::shared_ptr<TelemetrySink> {
  return std::make_shared<NoOpTelemetrySink>();
}

}  // namespace telepath
