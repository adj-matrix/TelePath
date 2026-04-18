#include <cassert>

#include "telepath/common/types.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

void RecordAllEvents(telepath::TelemetrySink *sink, const telepath::BufferTag &tag) {
  sink->RecordHit(tag);
  sink->RecordMiss(tag);
  sink->RecordDiskRead(tag);
  sink->RecordDiskWrite(tag);
  sink->RecordEviction(tag);
  sink->RecordDirtyFlush(tag);
}

void AssertSnapshotEmpty(const telepath::TelemetrySnapshot &snapshot) {
  assert(snapshot.buffer_hits == 0);
  assert(snapshot.buffer_misses == 0);
  assert(snapshot.disk_reads == 0);
  assert(snapshot.disk_writes == 0);
  assert(snapshot.evictions == 0);
  assert(snapshot.dirty_flushes == 0);
}

void AssertSnapshotHasSingleCountPerEvent(const telepath::TelemetrySnapshot &snapshot) {
  assert(snapshot.buffer_hits == 1);
  assert(snapshot.buffer_misses == 1);
  assert(snapshot.disk_reads == 1);
  assert(snapshot.disk_writes == 1);
  assert(snapshot.evictions == 1);
  assert(snapshot.dirty_flushes == 1);
}

void AssertNoOpTelemetrySinkIgnoresEvents(const telepath::BufferTag &tag) {
  auto sink = telepath::MakeNoOpTelemetrySink();
  RecordAllEvents(sink.get(), tag);
  AssertSnapshotEmpty(sink->Snapshot());
}

void AssertCounterTelemetrySinkTracksEvents(const telepath::BufferTag &tag) {
  auto sink = telepath::MakeCounterTelemetrySink();
  RecordAllEvents(sink.get(), tag);
  AssertSnapshotHasSingleCountPerEvent(sink->Snapshot());
}

}  // namespace

int main() {
  const telepath::BufferTag tag{1, 42};
  AssertNoOpTelemetrySinkIgnoresEvents(tag);
  AssertCounterTelemetrySinkTracksEvents(tag);
  return 0;
}
