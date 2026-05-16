#include "telepath/telemetry/telemetry_sink.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace telepath {

namespace {

auto JsonEscape(std::string_view input) -> std::string {
  std::string escaped;
  escaped.reserve(input.size());
  for (char ch : input) {
    switch (ch) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += ch;
      break;
    }
  }
  return escaped;
}

void AppendCountersJson(std::ostringstream *out, const TelemetrySnapshot &counters) {
  *out << "\"counters\":{";
  *out << "\"buffer_hits\":" << counters.buffer_hits << ",";
  *out << "\"buffer_misses\":" << counters.buffer_misses << ",";
  *out << "\"disk_reads\":" << counters.disk_reads << ",";
  *out << "\"disk_writes\":" << counters.disk_writes << ",";
  *out << "\"evictions\":" << counters.evictions << ",";
  *out << "\"dirty_flushes\":" << counters.dirty_flushes << ",";
  *out << "\"flush_tasks_scheduled\":" << counters.flush_tasks_scheduled << ",";
  *out << "\"flush_tasks_completed\":" << counters.flush_tasks_completed << ",";
  *out << "\"flush_failures\":" << counters.flush_failures << ",";
  *out << "\"cleaner_flushes_scheduled\":" << counters.cleaner_flushes_scheduled << ",";
  *out << "\"cleaner_flushes_finished\":" << counters.cleaner_flushes_finished << ",";
  *out << "\"cleaner_flushes_skipped\":" << counters.cleaner_flushes_skipped << ",";
  *out << "\"eviction_failures\":" << counters.eviction_failures;
  *out << "}";
}

void AppendFrameJson(std::ostringstream *out, const TelemetryExportFrame &frame) {
  *out << "{";
  *out << "\"frame_id\":" << frame.frame_id << ",";
  *out << "\"state\":\"" << JsonEscape(frame.state) << "\",";
  *out << "\"file_id\":" << frame.file_id << ",";
  *out << "\"block_id\":" << frame.block_id << ",";
  *out << "\"pin_count\":" << frame.pin_count << ",";
  *out << "\"dirty_generation\":" << frame.dirty_generation << ",";
  *out << "\"is_valid\":" << (frame.is_valid ? "true" : "false") << ",";
  *out << "\"is_dirty\":" << (frame.is_dirty ? "true" : "false") << ",";
  *out << "\"io_in_flight\":" << (frame.io_in_flight ? "true" : "false") << ",";
  *out << "\"flush_queued\":" << (frame.flush_queued ? "true" : "false") << ",";
  *out << "\"flush_in_flight\":" << (frame.flush_in_flight ? "true" : "false");
  *out << "}";
}

}  // namespace

auto SerializeTelemetryExportJson(const TelemetryExportSnapshot &snapshot) -> std::string {
  std::ostringstream out;
  out << "{";
  out << "\"timestamp_ms\":" << snapshot.timestamp_ms << ",";
  out << "\"source\":\"" << JsonEscape(snapshot.source) << "\",";
  out << "\"pool_size\":" << snapshot.pool_size << ",";
  out << "\"page_size\":" << snapshot.page_size << ",";
  out << "\"dirty_page_count\":" << snapshot.dirty_page_count << ",";
  out << "\"flush_queued_count\":" << snapshot.flush_queued_count << ",";
  out << "\"flush_in_flight_count\":" << snapshot.flush_in_flight_count << ",";
  AppendCountersJson(&out, snapshot.counters);
  out << ",\"frames\":[";
  for (std::size_t index = 0; index < snapshot.frames.size(); ++index) {
    AppendFrameJson(&out, snapshot.frames[index]);
    if (index + 1 < snapshot.frames.size()) out << ",";
  }
  out << "]}";
  return out.str();
}

auto AppendTelemetryExportJsonLine(const std::string &path, const TelemetryExportSnapshot &snapshot) -> Status {
  if (path.empty()) return Status::InvalidArgument("telemetry export path must not be empty");

  std::filesystem::path export_path(path);
  std::error_code create_error;
  if (export_path.has_parent_path()) {
    std::filesystem::create_directories(export_path.parent_path(), create_error);
    if (create_error) return Status::IoError("failed to create telemetry export directory: " + create_error.message());
  }

  std::ofstream out(export_path, std::ios::out | std::ios::app);
  if (!out.is_open()) return Status::IoError("failed to open telemetry export file");
  out << SerializeTelemetryExportJson(snapshot) << "\n";
  if (!out.good()) return Status::IoError("failed to write telemetry export file");
  return Status::Ok();
}

}  // namespace telepath
