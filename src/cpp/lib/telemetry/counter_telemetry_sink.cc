#include "telepath/telemetry/telemetry_sink.h"

#include <memory>

namespace telepath {

class CounterTelemetrySink : public TelemetrySink {
 public:
  CounterTelemetrySink() : TelemetrySink(&counters_) {}

 protected:
  void DoRecordHit(const BufferTag &) override {}
  void DoRecordMiss(const BufferTag &) override {}
  void DoRecordDiskRead(const BufferTag &) override {}
  void DoRecordDiskWrite(const BufferTag &) override {}
  void DoRecordEviction(const BufferTag &) override {}
  void DoRecordDirtyFlush(const BufferTag &) override {}

  TelemetrySnapshot DoSnapshot() const override { return TelemetrySnapshot{}; }

 private:
  TelemetryCounters counters_{};
};

std::shared_ptr<TelemetrySink> MakeCounterTelemetrySink() {
  return std::make_shared<CounterTelemetrySink>();
}

}  // namespace telepath
