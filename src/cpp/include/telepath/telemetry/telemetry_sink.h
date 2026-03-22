#ifndef TELEPATH_TELEMETRY_TELEMETRY_SINK_H_
#define TELEPATH_TELEMETRY_TELEMETRY_SINK_H_

#include <cstdint>
#include <atomic>
#include <memory>

#include "telepath/common/types.h"

namespace telepath {

struct TelemetrySnapshot {
  uint64_t buffer_hits{0};
  uint64_t buffer_misses{0};
  uint64_t disk_reads{0};
  uint64_t disk_writes{0};
  uint64_t evictions{0};
  uint64_t dirty_flushes{0};
};

// Fast-path counters used by in-process telemetry implementations that want to
// avoid virtual dispatch on the cache hot path.
struct TelemetryCounters {
  std::atomic<uint64_t> buffer_hits{0};
  std::atomic<uint64_t> buffer_misses{0};
  std::atomic<uint64_t> disk_reads{0};
  std::atomic<uint64_t> disk_writes{0};
  std::atomic<uint64_t> evictions{0};
  std::atomic<uint64_t> dirty_flushes{0};
};

class TelemetrySink {
 public:
  virtual ~TelemetrySink() = default;

  // Records a cache hit for the given page access.
  void RecordHit(const BufferTag &tag) {
    if (fast_path_counters_ != nullptr) {
      fast_path_counters_->buffer_hits.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    DoRecordHit(tag);
  }

  // Records a cache miss for the given page access.
  void RecordMiss(const BufferTag &tag) {
    if (fast_path_counters_ != nullptr) {
      fast_path_counters_->buffer_misses.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    DoRecordMiss(tag);
  }

  // Records a completed disk read.
  void RecordDiskRead(const BufferTag &tag) {
    if (fast_path_counters_ != nullptr) {
      fast_path_counters_->disk_reads.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    DoRecordDiskRead(tag);
  }

  // Records a completed disk write.
  void RecordDiskWrite(const BufferTag &tag) {
    if (fast_path_counters_ != nullptr) {
      fast_path_counters_->disk_writes.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    DoRecordDiskWrite(tag);
  }

  // Records an eviction of a resident page.
  void RecordEviction(const BufferTag &tag) {
    if (fast_path_counters_ != nullptr) {
      fast_path_counters_->evictions.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    DoRecordEviction(tag);
  }

  // Records a dirty-page flush event.
  void RecordDirtyFlush(const BufferTag &tag) {
    if (fast_path_counters_ != nullptr) {
      fast_path_counters_->dirty_flushes.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    DoRecordDirtyFlush(tag);
  }

  // Returns a point-in-time snapshot of all exported counters.
  TelemetrySnapshot Snapshot() const {
    if (fast_path_counters_ != nullptr) {
      return TelemetrySnapshot{
          fast_path_counters_->buffer_hits.load(std::memory_order_relaxed),
          fast_path_counters_->buffer_misses.load(std::memory_order_relaxed),
          fast_path_counters_->disk_reads.load(std::memory_order_relaxed),
          fast_path_counters_->disk_writes.load(std::memory_order_relaxed),
          fast_path_counters_->evictions.load(std::memory_order_relaxed),
          fast_path_counters_->dirty_flushes.load(std::memory_order_relaxed),
      };
    }
    return DoSnapshot();
  }

 protected:
  explicit TelemetrySink(TelemetryCounters *fast_path_counters = nullptr)
      : fast_path_counters_(fast_path_counters) {}

  virtual void DoRecordHit(const BufferTag &tag) = 0;
  virtual void DoRecordMiss(const BufferTag &tag) = 0;
  virtual void DoRecordDiskRead(const BufferTag &tag) = 0;
  virtual void DoRecordDiskWrite(const BufferTag &tag) = 0;
  virtual void DoRecordEviction(const BufferTag &tag) = 0;
  virtual void DoRecordDirtyFlush(const BufferTag &tag) = 0;
  virtual TelemetrySnapshot DoSnapshot() const = 0;

 private:
  TelemetryCounters *fast_path_counters_{nullptr};
};

// Returns a sink backed by in-process atomic counters.
std::shared_ptr<TelemetrySink> MakeCounterTelemetrySink();
// Returns a sink that intentionally drops all events.
std::shared_ptr<TelemetrySink> MakeNoOpTelemetrySink();

}  // namespace telepath

#endif  // TELEPATH_TELEMETRY_TELEMETRY_SINK_H_
