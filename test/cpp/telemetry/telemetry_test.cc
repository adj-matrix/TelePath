#include <cassert>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

auto BuildSharedMemoryName(const std::string &name) -> std::string {
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return "/telepath_" + name + "_" + std::to_string(unique_suffix);
}

auto BuildTelemetryExportSnapshot(const std::string &source, uint64_t hits) -> telepath::TelemetryExportSnapshot {
  telepath::TelemetryExportSnapshot snapshot;
  snapshot.timestamp_ms = 17;
  snapshot.source = source;
  snapshot.pool_size = 2;
  snapshot.page_size = 4096;
  snapshot.dirty_page_count = 1;
  snapshot.counters.buffer_hits = hits;
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
  return snapshot;
}

auto BuildSharedMemoryHeader(
  uint32_t magic,
  uint16_t version,
  uint16_t flags,
  uint64_t sequence,
  uint64_t payload_size,
  uint64_t payload_capacity
) -> telepath::TelemetrySharedMemoryHeader {
  telepath::TelemetrySharedMemoryHeader header;
  header.magic = magic;
  header.version = version;
  header.flags = flags;
  header.sequence = sequence;
  header.payload_size = payload_size;
  header.payload_capacity = payload_capacity;
  return header;
}

auto ReadSharedMemoryHeaderForTest(const std::string &name) -> telepath::TelemetrySharedMemoryHeader {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0);
  assert(fd >= 0);
  telepath::TelemetrySharedMemoryHeader header;
  const ssize_t read_size = pread(fd, &header, sizeof(header), 0);
  assert(read_size == static_cast<ssize_t>(sizeof(header)));
  assert(close(fd) == 0);
  return header;
}

void WriteRawSharedMemoryObjectForTest(
  const std::string &name,
  const telepath::TelemetrySharedMemoryHeader &header,
  const std::string &payload,
  std::size_t object_size
) {
  assert(telepath::UnlinkTelemetryExportSharedMemory(name).ok());
  const int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
  assert(fd >= 0);
  assert(ftruncate(fd, static_cast<off_t>(object_size)) == 0);

  if (object_size >= sizeof(header)) {
    const ssize_t header_write = pwrite(fd, &header, sizeof(header), 0);
    assert(header_write == static_cast<ssize_t>(sizeof(header)));
  }

  if (!payload.empty() && object_size > sizeof(header)) {
    const std::size_t writable_payload_size = std::min(payload.size(), object_size - sizeof(header));
    const ssize_t payload_write = pwrite(
      fd,
      payload.data(),
      writable_payload_size,
      static_cast<off_t>(sizeof(header)));
    assert(payload_write == static_cast<ssize_t>(writable_payload_size));
  }

  assert(close(fd) == 0);
}

void AssertTelemetryExportSerializesAndAppends() {
  telepath::TelemetryExportSnapshot snapshot = BuildTelemetryExportSnapshot("unit-test", 3);

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

void AssertTelemetryExportSharedMemoryRoundTrips() {
  const std::string name = BuildSharedMemoryName("telemetry_export");
  assert(telepath::UnlinkTelemetryExportSharedMemory(name).ok());

  const telepath::TelemetryExportSnapshot snapshot = BuildTelemetryExportSnapshot("unit-test-shm", 5);
  auto small_write = telepath::WriteTelemetryExportSharedMemory(name, 8, snapshot);
  assert(!small_write.ok());

  auto first_write = telepath::WriteTelemetryExportSharedMemory(name, 4096, snapshot);
  assert(first_write.ok());

  const telepath::TelemetrySharedMemoryHeader first_header = ReadSharedMemoryHeaderForTest(name);
  assert(first_header.magic == telepath::kTelemetrySharedMemoryMagic);
  assert(first_header.version == telepath::kTelemetrySharedMemoryVersion);
  assert((first_header.flags & telepath::kTelemetrySharedMemoryReady) != 0);
  assert(first_header.sequence == 1);
  assert(first_header.payload_capacity == 4096);
  assert(first_header.payload_size == telepath::SerializeTelemetryExportJson(snapshot).size());

  auto first_read = telepath::ReadTelemetryExportSharedMemory(name);
  assert(first_read.ok());
  assert(first_read.value().find("\"source\":\"unit-test-shm\"") != std::string::npos);
  assert(first_read.value().find("\"buffer_hits\":5") != std::string::npos);
  assert(first_read.value().find("\"frames\":[") != std::string::npos);

  const telepath::TelemetryExportSnapshot second_snapshot = BuildTelemetryExportSnapshot("unit-test-shm-second", 9);
  auto second_write = telepath::WriteTelemetryExportSharedMemory(name, 4096, second_snapshot);
  assert(second_write.ok());

  const telepath::TelemetrySharedMemoryHeader second_header = ReadSharedMemoryHeaderForTest(name);
  assert(second_header.sequence == 2);
  assert(second_header.payload_capacity == 4096);
  assert(second_header.payload_size == telepath::SerializeTelemetryExportJson(second_snapshot).size());

  auto second_read = telepath::ReadTelemetryExportSharedMemory(name);
  assert(second_read.ok());
  assert(second_read.value().find("\"source\":\"unit-test-shm-second\"") != std::string::npos);
  assert(second_read.value().find("\"buffer_hits\":9") != std::string::npos);

  assert(telepath::UnlinkTelemetryExportSharedMemory(name).ok());
  auto read_after_unlink = telepath::ReadTelemetryExportSharedMemory(name);
  assert(!read_after_unlink.ok());
}

void AssertTelemetryExportSharedMemoryRejectsInvalidNames() {
  const telepath::TelemetryExportSnapshot snapshot = BuildTelemetryExportSnapshot("unit-test-shm", 1);
  auto missing_slash = telepath::WriteTelemetryExportSharedMemory("telepath_invalid", 4096, snapshot);
  assert(!missing_slash.ok());
  auto zero_capacity = telepath::WriteTelemetryExportSharedMemory(BuildSharedMemoryName("zero_capacity"), 0, snapshot);
  assert(!zero_capacity.ok());
  auto nested_name = telepath::ReadTelemetryExportSharedMemory("/telepath/invalid");
  assert(!nested_name.ok());
}

void AssertTelemetryExportSharedMemoryRejectsMalformedObjects() {
  const std::string name = BuildSharedMemoryName("malformed_telemetry_export");
  const std::string payload = "{\"source\":\"raw-test\"}";

  const auto incompatible_header = BuildSharedMemoryHeader(
    0,
    telepath::kTelemetrySharedMemoryVersion,
    telepath::kTelemetrySharedMemoryReady,
    1,
    payload.size(),
    payload.size());
  WriteRawSharedMemoryObjectForTest(name, incompatible_header, payload, sizeof(incompatible_header) + payload.size());
  auto incompatible_read = telepath::ReadTelemetryExportSharedMemory(name);
  assert(!incompatible_read.ok());

  const auto not_ready_header = BuildSharedMemoryHeader(
    telepath::kTelemetrySharedMemoryMagic,
    telepath::kTelemetrySharedMemoryVersion,
    0,
    1,
    payload.size(),
    payload.size());
  WriteRawSharedMemoryObjectForTest(name, not_ready_header, payload, sizeof(not_ready_header) + payload.size());
  auto not_ready_read = telepath::ReadTelemetryExportSharedMemory(name);
  assert(!not_ready_read.ok());

  const auto payload_size_exceeds_capacity_header = BuildSharedMemoryHeader(
    telepath::kTelemetrySharedMemoryMagic,
    telepath::kTelemetrySharedMemoryVersion,
    telepath::kTelemetrySharedMemoryReady,
    1,
    4,
    1);
  WriteRawSharedMemoryObjectForTest(
    name,
    payload_size_exceeds_capacity_header,
    payload,
    sizeof(payload_size_exceeds_capacity_header) + payload.size());
  auto invalid_size_read = telepath::ReadTelemetryExportSharedMemory(name);
  assert(!invalid_size_read.ok());

  const auto capacity_exceeds_mapping_header = BuildSharedMemoryHeader(
    telepath::kTelemetrySharedMemoryMagic,
    telepath::kTelemetrySharedMemoryVersion,
    telepath::kTelemetrySharedMemoryReady,
    1,
    1,
    8);
  WriteRawSharedMemoryObjectForTest(
    name,
    capacity_exceeds_mapping_header,
    payload,
    sizeof(capacity_exceeds_mapping_header) + 1);
  auto invalid_capacity_read = telepath::ReadTelemetryExportSharedMemory(name);
  assert(!invalid_capacity_read.ok());

  WriteRawSharedMemoryObjectForTest(name, capacity_exceeds_mapping_header, "", sizeof(capacity_exceeds_mapping_header) - 1);
  auto tiny_object_read = telepath::ReadTelemetryExportSharedMemory(name);
  assert(!tiny_object_read.ok());

  assert(telepath::UnlinkTelemetryExportSharedMemory(name).ok());
}

}  // namespace

int main() {
  const telepath::BufferTag tag{1, 42};
  AssertNoOpTelemetrySinkIgnoresEvents(tag);
  AssertCounterTelemetrySinkTracksEvents(tag);
  AssertTelemetryExportSerializesAndAppends();
  AssertTelemetryExportEscapesJsonAndCreatesParentDirectories();
  AssertTelemetryExportSharedMemoryRoundTrips();
  AssertTelemetryExportSharedMemoryRejectsInvalidNames();
  AssertTelemetryExportSharedMemoryRejectsMalformedObjects();
  return 0;
}
