#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "benchmark_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend_factory.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

using telepath::benchmark_support::BenchmarkMetadata;
using telepath::benchmark_support::BenchmarkOptions;
using telepath::benchmark_support::ChooseBlock;
using telepath::benchmark_support::LoadMetadata;
using telepath::benchmark_support::NormalizeWorkloadName;
using telepath::benchmark_support::ParseArgs;

struct BenchmarkMetrics {
  std::size_t total_ops{0};
  double seconds{0.0};
  double throughput{0.0};
  uint64_t hits{0};
  uint64_t misses{0};
  double hit_rate{0.0};
};

class BenchmarkRootGuard {
 public:
  explicit BenchmarkRootGuard(std::filesystem::path root_path)
    : root_path_(std::move(root_path)) {}

  ~BenchmarkRootGuard() { std::filesystem::remove_all(root_path_); }

  auto path() const -> const std::filesystem::path & { return root_path_; }

 private:
  std::filesystem::path root_path_;
};

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

auto FrameStateName(telepath::BufferFrameState state) -> const char * {
  switch (state) {
  case telepath::BufferFrameState::kFree:
    return "free";
  case telepath::BufferFrameState::kLoading:
    return "loading";
  case telepath::BufferFrameState::kResident:
    return "resident";
  }
  return "unknown";
}

auto BuildBenchmarkRootPath() -> std::filesystem::path {
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() / ("telepath_benchmark_data_" + std::to_string(unique_suffix));
}

void PrepareBenchmarkRoot(const std::filesystem::path &root) {
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
}

auto BuildManagerOptions(
  const BenchmarkOptions &options,
  const telepath::DiskBackendOptions &disk_backend_options
) -> telepath::BufferManagerOptions {
  return telepath::BufferManagerOptions{
    options.pool_size,
    options.page_size,
    0,
    disk_backend_options,
  };
}

auto InitializeDiskBackend(
  const std::filesystem::path &root,
  const BenchmarkOptions &options,
  const telepath::DiskBackendOptions &disk_backend_options,
  std::unique_ptr<telepath::DiskBackend> *disk_backend,
  telepath::DiskBackendCapabilities *capabilities
) -> telepath::Status {
  auto disk_backend_result = telepath::CreateDiskBackend(root.string(), options.page_size, disk_backend_options);
  if (!disk_backend_result.ok()) return disk_backend_result.status();

  *capabilities = disk_backend_result.value()->GetCapabilities();
  *disk_backend = std::move(disk_backend_result.value());
  return telepath::Status::Ok();
}

auto PreloadBlock(
  telepath::BufferManager *manager,
  telepath::BlockId block_id
) -> telepath::Status {
  auto result = manager->ReadBuffer(1, block_id);
  if (!result.ok()) return telepath::Status::Internal("preload failed for block " + std::to_string(block_id) + ": " + result.status().message());

  telepath::BufferHandle handle = std::move(result.value());
  handle.mutable_data()[0] = static_cast<std::byte>(block_id % 251);
  auto dirty_status = manager->MarkBufferDirty(handle);
  if (!dirty_status.ok()) return telepath::Status::Internal("mark dirty failed for block " + std::to_string(block_id) + ": " + dirty_status.message());
  return telepath::Status::Ok();
}

auto PreloadBenchmarkData(
  telepath::BufferManager *manager,
  const BenchmarkOptions &options
) -> telepath::Status {
  for (telepath::BlockId block_id = 0; block_id < options.block_count; ++block_id) {
    auto preload_status = PreloadBlock(manager, block_id);
    if (!preload_status.ok()) return preload_status;
  }

  auto flush_status = manager->FlushAll();
  if (!flush_status.ok()) return telepath::Status::Internal("flush all failed after preload: " + flush_status.message());
  return telepath::Status::Ok();
}

void RunWorker(
  telepath::BufferManager *manager,
  const BenchmarkOptions &options,
  std::size_t thread_index,
  std::vector<uint64_t> *access_counts
) {
  std::mt19937_64 rng(0xBADC0FFEEULL + thread_index);
  for (std::size_t op = 0; op < options.ops_per_thread; ++op) {
    const telepath::BlockId block_id = ChooseBlock(&rng, op, thread_index, options);
    ++(*access_counts)[block_id];

    auto result = manager->ReadBuffer(1, block_id);
    if (!result.ok()) continue;

    telepath::BufferHandle handle = std::move(result.value());
    volatile std::byte sink = handle.data()[0];
    (void)sink;
  }
}

auto MergeAccessCounts(
  const std::vector<std::vector<uint64_t>> &per_thread_access_counts,
  std::size_t block_count
) -> std::vector<uint64_t> {
  std::vector<uint64_t> block_access_counts(block_count, 0);
  for (const auto &thread_counts : per_thread_access_counts) {
    for (std::size_t block_id = 0; block_id < thread_counts.size(); ++block_id) {
      block_access_counts[block_id] += thread_counts[block_id];
    }
  }
  return block_access_counts;
}

auto BuildBenchmarkMetrics(
  const BenchmarkOptions &options,
  const telepath::TelemetrySnapshot &before,
  const telepath::TelemetrySnapshot &after,
  double seconds
) -> BenchmarkMetrics {
  const std::size_t total_ops = options.thread_count * options.ops_per_thread;
  const uint64_t hits = after.buffer_hits - before.buffer_hits;
  const uint64_t misses = after.buffer_misses - before.buffer_misses;
  const double throughput = seconds > 0.0 ? total_ops / seconds : 0.0;
  const double hit_rate = (hits + misses) > 0 ? static_cast<double>(hits) / (hits + misses) : 0.0;
  return BenchmarkMetrics{
    total_ops,
    seconds,
    throughput,
    hits,
    misses,
    hit_rate,
  };
}

auto RunBenchmark(
  telepath::BufferManager *manager,
  const std::shared_ptr<telepath::TelemetrySink> &telemetry,
  const BenchmarkOptions &options,
  std::vector<uint64_t> *block_access_counts
) -> BenchmarkMetrics {
  const telepath::TelemetrySnapshot before = telemetry->Snapshot();
  const auto start = std::chrono::steady_clock::now();

  std::vector<std::thread> workers;
  workers.reserve(options.thread_count);
  std::vector<std::vector<uint64_t>> per_thread_access_counts(options.thread_count, std::vector<uint64_t>(options.block_count, 0));

  for (std::size_t thread_index = 0; thread_index < options.thread_count; ++thread_index) {
    workers.emplace_back(
      [manager, &options, &per_thread_access_counts, thread_index]() {
      RunWorker(manager, options, thread_index, &per_thread_access_counts[thread_index]);
    });
  }

  for (auto &worker : workers) worker.join();

  const auto end = std::chrono::steady_clock::now();
  const telepath::TelemetrySnapshot after = telemetry->Snapshot();
  const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  *block_access_counts = MergeAccessCounts(per_thread_access_counts, options.block_count);
  return BuildBenchmarkMetrics(options, before, after, seconds);
}

auto ResolveBackendKindName(const telepath::DiskBackendCapabilities &capabilities) -> const char * {
  if (capabilities.kind == telepath::DiskBackendKind::kIoUring) {
    if (capabilities.is_fallback_backend) return "io_uring_fallback";
    return "io_uring";
  }
  if (capabilities.is_fallback_backend) return "posix_fallback";
  return "posix";
}

void PrintTextSummary(
  const BenchmarkOptions &options,
  const BenchmarkMetadata &metadata,
  const char *backend_kind,
  const BenchmarkMetrics &metrics
) {
  std::cout << "telepath_benchmark\n";
  std::cout << "workload=" << NormalizeWorkloadName(options.workload) << "\n";
  std::cout << "disk_backend=" << backend_kind << "\n";
  std::cout << "commit_sha=" << metadata.commit_sha << "\n";
  std::cout << "runner_os=" << metadata.runner_os << "\n";
  std::cout << "runner_arch=" << metadata.runner_arch << "\n";
  std::cout << "threads=" << options.thread_count << "\n";
  std::cout << "pool_size=" << options.pool_size << "\n";
  std::cout << "block_count=" << options.block_count << "\n";
  std::cout << "ops_per_thread=" << options.ops_per_thread << "\n";
  std::cout << "hotset_size=" << options.hotset_size << "\n";
  std::cout << "hot_access_percent=" << options.hot_access_percent << "\n";
  std::cout << "total_ops=" << metrics.total_ops << "\n";
  std::cout << "seconds=" << metrics.seconds << "\n";
  std::cout << "throughput_ops_per_sec=" << metrics.throughput << "\n";
  std::cout << "buffer_hits=" << metrics.hits << "\n";
  std::cout << "buffer_misses=" << metrics.misses << "\n";
  std::cout << "hit_rate=" << metrics.hit_rate << "\n";
}

void PrintCsvSummary(
  const BenchmarkOptions &options,
  const BenchmarkMetadata &metadata,
  const char *backend_kind,
  const BenchmarkMetrics &metrics
) {
  std::cout
      << "workload,disk_backend,commit_sha,runner_os,runner_arch,threads,pool_size,"
         "block_count,ops_per_thread,hotset_size,hot_access_percent,total_ops,"
         "seconds,throughput_ops_per_sec,buffer_hits,buffer_misses,hit_rate\n";
  std::cout << NormalizeWorkloadName(options.workload) << "," << backend_kind
            << "," << metadata.commit_sha << "," << metadata.runner_os << ","
            << metadata.runner_arch << "," << options.thread_count << ","
            << options.pool_size << "," << options.block_count << ","
            << options.ops_per_thread << "," << options.hotset_size << ","
            << options.hot_access_percent << "," << metrics.total_ops << ","
            << metrics.seconds << "," << metrics.throughput << ","
            << metrics.hits << "," << metrics.misses << ","
            << metrics.hit_rate << "\n";
}

void PrintJsonMetrics(
  const BenchmarkOptions &options,
  const BenchmarkMetadata &metadata,
  const char *backend_kind,
  const BenchmarkMetrics &metrics
) {
  std::cout << "  \"metrics\": {\n";
  std::cout << "    \"workload\": \""
            << JsonEscape(NormalizeWorkloadName(options.workload)) << "\",\n";
  std::cout << "    \"disk_backend\": \"" << JsonEscape(backend_kind)
            << "\",\n";
  std::cout << "    \"commit_sha\": \"" << JsonEscape(metadata.commit_sha)
            << "\",\n";
  std::cout << "    \"runner_os\": \"" << JsonEscape(metadata.runner_os)
            << "\",\n";
  std::cout << "    \"runner_arch\": \"" << JsonEscape(metadata.runner_arch)
            << "\",\n";
  std::cout << "    \"threads\": " << options.thread_count << ",\n";
  std::cout << "    \"pool_size\": " << options.pool_size << ",\n";
  std::cout << "    \"block_count\": " << options.block_count << ",\n";
  std::cout << "    \"ops_per_thread\": " << options.ops_per_thread << ",\n";
  std::cout << "    \"hotset_size\": " << options.hotset_size << ",\n";
  std::cout << "    \"hot_access_percent\": " << options.hot_access_percent
            << ",\n";
  std::cout << "    \"total_ops\": " << metrics.total_ops << ",\n";
  std::cout << "    \"seconds\": " << metrics.seconds << ",\n";
  std::cout << "    \"throughput_ops_per_sec\": " << metrics.throughput
            << ",\n";
  std::cout << "    \"buffer_hits\": " << metrics.hits << ",\n";
  std::cout << "    \"buffer_misses\": " << metrics.misses << ",\n";
  std::cout << "    \"hit_rate\": " << metrics.hit_rate << "\n";
  std::cout << "  },\n";
}

void PrintJsonFrame(
  const telepath::FrameSnapshot &frame,
  bool has_next_frame
) {
  std::cout << "      {\n";
  std::cout << "        \"frame_id\": " << frame.frame_id << ",\n";
  std::cout << "        \"state\": \"" << FrameStateName(frame.state)
            << "\",\n";
  std::cout << "        \"file_id\": " << frame.tag.file_id << ",\n";
  std::cout << "        \"block_id\": " << frame.tag.block_id << ",\n";
  std::cout << "        \"pin_count\": " << frame.pin_count << ",\n";
  std::cout << "        \"dirty_generation\": " << frame.dirty_generation
            << ",\n";
  std::cout << "        \"is_valid\": "
            << (frame.is_valid ? "true" : "false") << ",\n";
  std::cout << "        \"is_dirty\": "
            << (frame.is_dirty ? "true" : "false") << ",\n";
  std::cout << "        \"io_in_flight\": "
            << (frame.io_in_flight ? "true" : "false") << ",\n";
  std::cout << "        \"flush_queued\": "
            << (frame.flush_queued ? "true" : "false") << ",\n";
  std::cout << "        \"flush_in_flight\": "
            << (frame.flush_in_flight ? "true" : "false") << "\n";
  std::cout << "      }";
  if (has_next_frame) std::cout << ",";
  std::cout << "\n";
}

void PrintJsonSnapshot(const telepath::BufferPoolSnapshot &snapshot) {
  std::cout << "  \"snapshot\": {\n";
  std::cout << "    \"pool_size\": " << snapshot.pool_size << ",\n";
  std::cout << "    \"page_size\": " << snapshot.page_size << ",\n";
  std::cout << "    \"frames\": [\n";
  for (std::size_t index = 0; index < snapshot.frames.size(); ++index) {
    PrintJsonFrame(snapshot.frames[index], index + 1 < snapshot.frames.size());
  }
  std::cout << "    ]\n";
  std::cout << "  },\n";
}

void PrintJsonAccessEntry(
  std::size_t block_id,
  uint64_t accesses,
  std::size_t total_ops,
  bool has_next_block
) {
  const double share = total_ops > 0 ? static_cast<double>(accesses) / static_cast<double>(total_ops) : 0.0;
  std::cout << "      {\n";
  std::cout << "        \"block_id\": " << block_id << ",\n";
  std::cout << "        \"accesses\": " << accesses << ",\n";
  std::cout << "        \"share\": " << share << "\n";
  std::cout << "      }";
  if (has_next_block) std::cout << ",";
  std::cout << "\n";
}

void PrintJsonAccessProfile(
  std::size_t total_ops,
  const std::vector<uint64_t> &block_access_counts
) {
  std::cout << "  \"access_profile\": {\n";
  std::cout << "    \"mode\": \"observed\",\n";
  std::cout << "    \"blocks\": [\n";
  for (std::size_t block_id = 0; block_id < block_access_counts.size(); ++block_id) {
    PrintJsonAccessEntry(block_id, block_access_counts[block_id], total_ops, block_id + 1 < block_access_counts.size());
  }
  std::cout << "    ]\n";
  std::cout << "  }\n";
}

void PrintJsonSummary(
  const BenchmarkOptions &options,
  const BenchmarkMetadata &metadata,
  const char *backend_kind,
  const BenchmarkMetrics &metrics,
  const telepath::BufferPoolSnapshot &snapshot,
  const std::vector<uint64_t> &block_access_counts
) {
  std::cout << "{\n";
  PrintJsonMetrics(options, metadata, backend_kind, metrics);
  PrintJsonSnapshot(snapshot);
  PrintJsonAccessProfile(metrics.total_ops, block_access_counts);
  std::cout << "}\n";
}

void PrintSummary(
  const BenchmarkOptions &options,
  const BenchmarkMetadata &metadata,
  const char *backend_kind,
  const BenchmarkMetrics &metrics,
  const telepath::BufferPoolSnapshot &snapshot,
  const std::vector<uint64_t> &block_access_counts
) {
  if (options.output_format == "csv") {
    PrintCsvSummary(options, metadata, backend_kind, metrics);
    return;
  }
  if (options.output_format == "json") {
    PrintJsonSummary(options, metadata, backend_kind, metrics, snapshot, block_access_counts);
    return;
  }
  PrintTextSummary(options, metadata, backend_kind, metrics);
}

}  // namespace

int main(int argc, char **argv) {
  const BenchmarkOptions options = ParseArgs(argc, argv);
  const BenchmarkMetadata metadata = LoadMetadata();
  BenchmarkRootGuard root_guard(BuildBenchmarkRootPath());
  PrepareBenchmarkRoot(root_guard.path());

  auto telemetry = telepath::MakeCounterTelemetrySink();
  const telepath::DiskBackendOptions disk_backend_options{};
  std::unique_ptr<telepath::DiskBackend> disk_backend;
  telepath::DiskBackendCapabilities disk_backend_capabilities;
  auto disk_backend_status = InitializeDiskBackend(root_guard.path(), options, disk_backend_options, &disk_backend, &disk_backend_capabilities);
  if (!disk_backend_status.ok()) {
    std::cerr << "backend creation failed: " << disk_backend_status.message() << "\n";
    return 1;
  }

  auto replacer = telepath::MakeLruKReplacer(options.pool_size, 2);
  const telepath::BufferManagerOptions manager_options = BuildManagerOptions(options, disk_backend_options);
  telepath::BufferManager manager(manager_options, std::move(disk_backend), std::move(replacer), telemetry);

  auto preload_status = PreloadBenchmarkData(&manager, options);
  if (!preload_status.ok()) {
    std::cerr << preload_status.message() << "\n";
    return 1;
  }

  std::vector<uint64_t> block_access_counts;
  const BenchmarkMetrics metrics = RunBenchmark(&manager, telemetry, options, &block_access_counts);
  const telepath::BufferPoolSnapshot snapshot = manager.ExportSnapshot();
  PrintSummary(options, metadata, ResolveBackendKindName(disk_backend_capabilities), metrics, snapshot, block_access_counts);
  return 0;
}
