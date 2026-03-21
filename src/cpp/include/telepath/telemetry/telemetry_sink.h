#ifndef TELEPATH_TELEMETRY_TELEMETRY_SINK_H_
#define TELEPATH_TELEMETRY_TELEMETRY_SINK_H_

#include <cstdint>
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

class TelemetrySink {
 public:
  virtual ~TelemetrySink() = default;

  virtual void RecordHit(const BufferTag &tag) = 0;
  virtual void RecordMiss(const BufferTag &tag) = 0;
  virtual void RecordDiskRead(const BufferTag &tag) = 0;
  virtual void RecordDiskWrite(const BufferTag &tag) = 0;
  virtual void RecordEviction(const BufferTag &tag) = 0;
  virtual void RecordDirtyFlush(const BufferTag &tag) = 0;
  virtual TelemetrySnapshot Snapshot() const = 0;
};

std::shared_ptr<TelemetrySink> MakeCounterTelemetrySink();
std::shared_ptr<TelemetrySink> MakeNoOpTelemetrySink();

}  // namespace telepath

#endif  // TELEPATH_TELEMETRY_TELEMETRY_SINK_H_
