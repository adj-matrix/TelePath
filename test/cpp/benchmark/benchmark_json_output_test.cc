#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

namespace {

struct BenchmarkCommand {
  std::string command;
  std::filesystem::path export_path;
};

auto BuildBenchmarkCommand(const std::filesystem::path &benchmark_bin) -> BenchmarkCommand {
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path export_path = std::filesystem::temp_directory_path() / ("telepath_benchmark_json_output_export_" + std::to_string(unique_suffix) + ".jsonl");
  std::filesystem::remove(export_path);
  BenchmarkCommand command;
  command.export_path = export_path;
  command.command = benchmark_bin.string() +
                    " --output-format json --workload hotspot --pool-size 16"
                    " --block-count 32 --threads 2 --ops-per-thread 12"
                    " --hotset-size 8 --hot-access-percent 80"
                    " --replacer clock --disk-backend posix"
                    " --write-percent 25 --flush-every-ops 6"
                    " --background-cleaner true --dirty-page-high-watermark 12"
                    " --dirty-page-low-watermark 4 --flush-workers 2"
                    " --flush-submit-batch-size 2 --max-open-files 8"
                    " --telemetry-export-path " + export_path.string();
  return command;
}

auto ReadProcessOutput(const std::string &command, int *exit_code) -> std::string {
  FILE *pipe = popen(command.c_str(), "r");
  assert(pipe != nullptr);

  std::string output;
  std::array<char, 512> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  *exit_code = pclose(pipe);
  return output;
}

auto ExtractTotalOps(const std::string &output) -> uint64_t {
  const std::regex total_ops_pattern(R"("total_ops":\s*([0-9]+))");
  std::smatch total_ops_match;
  assert(std::regex_search(output, total_ops_match, total_ops_pattern));
  return std::stoull(total_ops_match[1].str());
}

auto ExtractUint64Metric(const std::string &output, const std::string &name) -> uint64_t {
  const std::regex metric_pattern("\"" + name + R"(":\s*([0-9]+))");
  std::smatch metric_match;
  assert(std::regex_search(output, metric_match, metric_pattern));
  return std::stoull(metric_match[1].str());
}

auto SumObservedAccesses(const std::string &output) -> uint64_t {
  const std::regex access_pattern(R"("accesses":\s*([0-9]+))");
  uint64_t access_sum = 0;
  for (std::sregex_iterator iter(output.begin(), output.end(), access_pattern); iter != std::sregex_iterator(); ++iter) {
    access_sum += std::stoull((*iter)[1].str());
  }
  return access_sum;
}

void AssertTelemetryExportFileWritten(const std::filesystem::path &path) {
  assert(std::filesystem::exists(path));
  std::ifstream in(path);
  assert(in.is_open());

  std::string line;
  std::size_t line_count = 0;
  while (std::getline(in, line)) {
    assert(!line.empty());
    assert(line.find("\"source\":\"telepath_benchmark\"") != std::string::npos);
    assert(line.find("\"frames\":[") != std::string::npos);
    assert(line.find("\"dirty_page_count\"") != std::string::npos);
    ++line_count;
  }
  assert(line_count == 1);
  std::filesystem::remove(path);
}

}  // namespace

int main(int /*argc*/, char **argv) {
  namespace fs = std::filesystem;

  const fs::path executable_dir = fs::path(argv[0]).parent_path();
  const fs::path benchmark_bin = executable_dir / "telepath_benchmark";
  const BenchmarkCommand command = BuildBenchmarkCommand(benchmark_bin);

  int exit_code = 0;
  const std::string output = ReadProcessOutput(command.command, &exit_code);
  assert(exit_code == 0);

  assert(output.find("\"access_profile\"") != std::string::npos);
  assert(output.find("\"mode\": \"observed\"") != std::string::npos);
  assert(output.find("\"replacer\": \"clock\"") != std::string::npos);
  assert(output.find("\"requested_disk_backend\": \"posix\"") != std::string::npos);
  assert(output.find("\"write_percent\": 25") != std::string::npos);
  assert(output.find("\"writes_marked_dirty\"") != std::string::npos);
  assert(output.find("\"foreground_flushes_requested\"") != std::string::npos);
  assert(output.find("\"operation_latency_avg_ns\"") != std::string::npos);
  assert(output.find("\"telemetry_export_enabled\": true") != std::string::npos);
  assert(output.find("\"flush_tasks_scheduled\"") != std::string::npos);
  assert(output.find("\"dirty_page_count\"") != std::string::npos);
  assert(output.find("\"flush_queued_count\"") != std::string::npos);

  const uint64_t total_ops = ExtractTotalOps(output);
  const uint64_t access_sum = SumObservedAccesses(output);
  assert(access_sum == total_ops);
  const uint64_t min_latency = ExtractUint64Metric(output, "operation_latency_min_ns");
  const uint64_t p50_latency = ExtractUint64Metric(output, "operation_latency_p50_ns");
  const uint64_t p95_latency = ExtractUint64Metric(output, "operation_latency_p95_ns");
  const uint64_t p99_latency = ExtractUint64Metric(output, "operation_latency_p99_ns");
  const uint64_t max_latency = ExtractUint64Metric(output, "operation_latency_max_ns");
  assert(min_latency <= p50_latency);
  assert(p50_latency <= p95_latency);
  assert(p95_latency <= p99_latency);
  assert(p99_latency <= max_latency);
  AssertTelemetryExportFileWritten(command.export_path);
  return 0;
}
