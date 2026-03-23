#include <cassert>

#include "telepath/common/types.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  const telepath::BufferTag tag{1, 42};

  auto no_op_sink = telepath::MakeNoOpTelemetrySink();
  no_op_sink->RecordHit(tag);
  no_op_sink->RecordMiss(tag);
  no_op_sink->RecordDiskRead(tag);
  no_op_sink->RecordDiskWrite(tag);
  no_op_sink->RecordEviction(tag);
  no_op_sink->RecordDirtyFlush(tag);
  const telepath::TelemetrySnapshot no_op_snapshot = no_op_sink->Snapshot();
  assert(no_op_snapshot.buffer_hits == 0);
  assert(no_op_snapshot.buffer_misses == 0);
  assert(no_op_snapshot.disk_reads == 0);
  assert(no_op_snapshot.disk_writes == 0);
  assert(no_op_snapshot.evictions == 0);
  assert(no_op_snapshot.dirty_flushes == 0);

  auto counter_sink = telepath::MakeCounterTelemetrySink();
  counter_sink->RecordHit(tag);
  counter_sink->RecordMiss(tag);
  counter_sink->RecordDiskRead(tag);
  counter_sink->RecordDiskWrite(tag);
  counter_sink->RecordEviction(tag);
  counter_sink->RecordDirtyFlush(tag);
  const telepath::TelemetrySnapshot counter_snapshot = counter_sink->Snapshot();
  assert(counter_snapshot.buffer_hits == 1);
  assert(counter_snapshot.buffer_misses == 1);
  assert(counter_snapshot.disk_reads == 1);
  assert(counter_snapshot.disk_writes == 1);
  assert(counter_snapshot.evictions == 1);
  assert(counter_snapshot.dirty_flushes == 1);

  return 0;
}
