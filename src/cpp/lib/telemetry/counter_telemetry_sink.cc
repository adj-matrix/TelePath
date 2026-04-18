#include "telepath/telemetry/telemetry_sink.h"

#include <memory>

namespace telepath {

class CounterTelemetrySink final : public TelemetrySink {
 public:
  CounterTelemetrySink() : TelemetrySink(&counters_) {}

 protected:
  void DoRecordHit(const BufferTag &) override {}
  void DoRecordMiss(const BufferTag &) override {}
  void DoRecordDiskRead(const BufferTag &) override {}
  void DoRecordDiskWrite(const BufferTag &) override {}
  void DoRecordEviction(const BufferTag &) override {}
  void DoRecordDirtyFlush(const BufferTag &) override {}

  auto DoSnapshot() const -> TelemetrySnapshot override {
    return ReadCountersSnapshot(counters_);
  }

 private:
  TelemetryCounters counters_{};
};

auto MakeCounterTelemetrySink() -> std::shared_ptr<TelemetrySink> {
  return std::make_shared<CounterTelemetrySink>();
}

}  // namespace telepath
