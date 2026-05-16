#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
  sink->RecordFlushTaskScheduled(tag);
  sink->RecordFlushTaskCompleted(tag);
  sink->RecordFlushFailure(tag);
  sink->RecordCleanerFlushScheduled(tag);
  sink->RecordCleanerFlushFinished(tag);
  sink->RecordCleanerFlushSkipped();
  sink->RecordEvictionFailure(tag);
}

void AssertSnapshotEmpty(const telepath::TelemetrySnapshot &snapshot) {
  assert(snapshot.buffer_hits == 0);
  assert(snapshot.buffer_misses == 0);
  assert(snapshot.disk_reads == 0);
  assert(snapshot.disk_writes == 0);
  assert(snapshot.evictions == 0);
  assert(snapshot.dirty_flushes == 0);
  assert(snapshot.flush_tasks_scheduled == 0);
  assert(snapshot.flush_tasks_completed == 0);
  assert(snapshot.flush_failures == 0);
  assert(snapshot.cleaner_flushes_scheduled == 0);
  assert(snapshot.cleaner_flushes_finished == 0);
  assert(snapshot.cleaner_flushes_skipped == 0);
  assert(snapshot.eviction_failures == 0);
}

void AssertSnapshotHasSingleCountPerEvent(const telepath::TelemetrySnapshot &snapshot) {
  assert(snapshot.buffer_hits == 1);
  assert(snapshot.buffer_misses == 1);
  assert(snapshot.disk_reads == 1);
  assert(snapshot.disk_writes == 1);
  assert(snapshot.evictions == 1);
  assert(snapshot.dirty_flushes == 1);
  assert(snapshot.flush_tasks_scheduled == 1);
  assert(snapshot.flush_tasks_completed == 1);
  assert(snapshot.flush_failures == 1);
  assert(snapshot.cleaner_flushes_scheduled == 1);
  assert(snapshot.cleaner_flushes_finished == 1);
  assert(snapshot.cleaner_flushes_skipped == 1);
  assert(snapshot.eviction_failures == 1);
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

auto BuildExportPath() -> std::filesystem::path {
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() / ("telepath_telemetry_export_" + std::to_string(unique_suffix) + ".jsonl");
}

auto BuildNestedExportPath() -> std::filesystem::path {
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() / ("telepath_telemetry_export_nested_" + std::to_string(unique_suffix)) / "subdir" / "export.jsonl";
}

void AssertTelemetryExportSerializesAndAppends() {
  telepath::TelemetryExportSnapshot snapshot;
  snapshot.timestamp_ms = 17;
  snapshot.source = "unit-test";
  snapshot.pool_size = 2;
  snapshot.page_size = 4096;
  snapshot.dirty_page_count = 1;
  snapshot.counters.buffer_hits = 3;
  snapshot.counters.flush_tasks_scheduled = 4;
  snapshot.frames.push_back(telepath::TelemetryExportFrame{
    1,
    7,
    9,
    0,
    2,
    true,
    true,
    false,
    true,
    false,
    "resident",
  });

  const std::string json = telepath::SerializeTelemetryExportJson(snapshot);
  assert(json.find("\"source\":\"unit-test\"") != std::string::npos);
  assert(json.find("\"buffer_hits\":3") != std::string::npos);
  assert(json.find("\"flush_tasks_scheduled\":4") != std::string::npos);
  assert(json.find("\"state\":\"resident\"") != std::string::npos);
  assert(json.find("\"flush_queued\":true") != std::string::npos);

  const std::filesystem::path path = BuildExportPath();
  auto first_append = telepath::AppendTelemetryExportJsonLine(path.string(), snapshot);
  assert(first_append.ok());
  auto second_append = telepath::AppendTelemetryExportJsonLine(path.string(), snapshot);
  assert(second_append.ok());

  std::ifstream in(path);
  assert(in.is_open());
  std::string line;
  std::size_t line_count = 0;
  while (std::getline(in, line)) {
    assert(line.find("\"source\":\"unit-test\"") != std::string::npos);
    ++line_count;
  }
  assert(line_count == 2);
  std::filesystem::remove(path);
}

void AssertTelemetryExportEscapesJsonAndCreatesParentDirectories() {
  telepath::TelemetryExportSnapshot snapshot;
  snapshot.source = "unit\"test\\source\n";
  snapshot.frames.push_back(telepath::TelemetryExportFrame{
    0,
    1,
    2,
    0,
    0,
    true,
    false,
    false,
    false,
    false,
    "needs\"escape\\newline\n",
  });

  const std::string json = telepath::SerializeTelemetryExportJson(snapshot);
  assert(json.find("\"source\":\"unit\\\"test\\\\source\\n\"") != std::string::npos);
  assert(json.find("\"state\":\"needs\\\"escape\\\\newline\\n\"") != std::string::npos);

  const std::filesystem::path path = BuildNestedExportPath();
  const std::filesystem::path parent = path.parent_path();
  std::filesystem::remove(path);
  std::filesystem::remove_all(parent.parent_path());

  auto append = telepath::AppendTelemetryExportJsonLine(path.string(), snapshot);
  assert(append.ok());
  assert(std::filesystem::exists(path));

  std::ifstream in(path);
  assert(in.is_open());
  std::string line;
  assert(std::getline(in, line));
  assert(line == json);

  std::filesystem::remove(path);
  std::filesystem::remove_all(parent.parent_path());
}

}  // namespace

int main() {
  const telepath::BufferTag tag{1, 42};
  AssertNoOpTelemetrySinkIgnoresEvents(tag);
  AssertCounterTelemetrySinkTracksEvents(tag);
  AssertTelemetryExportSerializesAndAppends();
  AssertTelemetryExportEscapesJsonAndCreatesParentDirectories();
  return 0;
}
