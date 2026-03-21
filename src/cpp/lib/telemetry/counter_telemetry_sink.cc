#include "telepath/telemetry/telemetry_sink.h"

#include <atomic>
#include <memory>

namespace telepath {

class CounterTelemetrySink : public TelemetrySink {
 public:
  void RecordHit(const BufferTag &) override { ++buffer_hits_; }
  void RecordMiss(const BufferTag &) override { ++buffer_misses_; }
  void RecordDiskRead(const BufferTag &) override { ++disk_reads_; }
  void RecordDiskWrite(const BufferTag &) override { ++disk_writes_; }
  void RecordEviction(const BufferTag &) override { ++evictions_; }
  void RecordDirtyFlush(const BufferTag &) override { ++dirty_flushes_; }

  TelemetrySnapshot Snapshot() const override {
    return TelemetrySnapshot{
        buffer_hits_.load(),
        buffer_misses_.load(),
        disk_reads_.load(),
        disk_writes_.load(),
        evictions_.load(),
        dirty_flushes_.load(),
    };
  }

 private:
  std::atomic<uint64_t> buffer_hits_{0};
  std::atomic<uint64_t> buffer_misses_{0};
  std::atomic<uint64_t> disk_reads_{0};
  std::atomic<uint64_t> disk_writes_{0};
  std::atomic<uint64_t> evictions_{0};
  std::atomic<uint64_t> dirty_flushes_{0};
};

std::shared_ptr<TelemetrySink> MakeCounterTelemetrySink() {
  return std::make_shared<CounterTelemetrySink>();
}

}  // namespace telepath
