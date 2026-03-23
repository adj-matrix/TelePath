#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"
#include "benchmark_support.h"

namespace {
using telepath::benchmark_support::BenchmarkMetadata;
using telepath::benchmark_support::BenchmarkOptions;
using telepath::benchmark_support::ChooseBlock;
using telepath::benchmark_support::LoadMetadata;
using telepath::benchmark_support::NormalizeWorkloadName;
using telepath::benchmark_support::ParseArgs;

void PrintTextSummary(const BenchmarkOptions &options,
                      const BenchmarkMetadata &metadata,
                      std::size_t total_ops, double seconds, double throughput,
                      uint64_t hits, uint64_t misses, double hit_rate) {
  std::cout << "telepath_benchmark\n";
  std::cout << "workload=" << NormalizeWorkloadName(options.workload) << "\n";
  std::cout << "commit_sha=" << metadata.commit_sha << "\n";
  std::cout << "runner_os=" << metadata.runner_os << "\n";
  std::cout << "runner_arch=" << metadata.runner_arch << "\n";
  std::cout << "threads=" << options.thread_count << "\n";
  std::cout << "pool_size=" << options.pool_size << "\n";
  std::cout << "block_count=" << options.block_count << "\n";
  std::cout << "ops_per_thread=" << options.ops_per_thread << "\n";
  std::cout << "hotset_size=" << options.hotset_size << "\n";
  std::cout << "hot_access_percent=" << options.hot_access_percent << "\n";
  std::cout << "total_ops=" << total_ops << "\n";
  std::cout << "seconds=" << seconds << "\n";
  std::cout << "throughput_ops_per_sec=" << throughput << "\n";
  std::cout << "buffer_hits=" << hits << "\n";
  std::cout << "buffer_misses=" << misses << "\n";
  std::cout << "hit_rate=" << hit_rate << "\n";
}

void PrintCsvSummary(const BenchmarkOptions &options,
                     const BenchmarkMetadata &metadata,
                     std::size_t total_ops, double seconds, double throughput,
                     uint64_t hits, uint64_t misses, double hit_rate) {
  std::cout
      << "workload,commit_sha,runner_os,runner_arch,threads,pool_size,"
         "block_count,ops_per_thread,hotset_size,hot_access_percent,total_ops,"
         "seconds,throughput_ops_per_sec,buffer_hits,buffer_misses,hit_rate\n";
  std::cout << NormalizeWorkloadName(options.workload) << ","
            << metadata.commit_sha << ","
            << metadata.runner_os << ","
            << metadata.runner_arch << ","
            << options.thread_count << ","
            << options.pool_size << ","
            << options.block_count << ","
            << options.ops_per_thread << ","
            << options.hotset_size << ","
            << options.hot_access_percent << ","
            << total_ops << ","
            << seconds << ","
            << throughput << ","
            << hits << ","
            << misses << ","
            << hit_rate << "\n";
}

}  // namespace

int main(int argc, char **argv) {
  namespace fs = std::filesystem;
  using telepath::BlockId;
  using telepath::BufferManager;
  using telepath::PosixDiskBackend;

  const BenchmarkOptions options = ParseArgs(argc, argv);
  const BenchmarkMetadata metadata = LoadMetadata();
  const auto unique_suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root =
      fs::temp_directory_path() /
      ("telepath_benchmark_data_" + std::to_string(unique_suffix));
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend =
      std::make_unique<PosixDiskBackend>(root.string(), options.page_size);
  auto replacer = telepath::MakeLruKReplacer(options.pool_size, 2);
  const telepath::BufferManagerOptions manager_options{
      options.pool_size, options.page_size, 0};
  BufferManager manager(manager_options, std::move(disk_backend),
                        std::move(replacer), telemetry);

  for (BlockId block_id = 0; block_id < options.block_count; ++block_id) {
    auto result = manager.ReadBuffer(1, block_id);
    if (!result.ok()) {
      std::cerr << "preload failed for block " << block_id << ": "
                << result.status().message() << "\n";
      return 1;
    }
    telepath::BufferHandle handle = std::move(result.value());
    handle.mutable_data()[0] = static_cast<std::byte>(block_id % 251);
    if (!manager.MarkBufferDirty(handle).ok()) {
      std::cerr << "mark dirty failed during preload\n";
      return 1;
    }
  }
  if (!manager.FlushAll().ok()) {
    std::cerr << "flush all failed after preload\n";
    return 1;
  }

  const telepath::TelemetrySnapshot before = telemetry->Snapshot();
  const auto start = std::chrono::steady_clock::now();

  std::vector<std::thread> workers;
  workers.reserve(options.thread_count);
  for (std::size_t thread_index = 0; thread_index < options.thread_count;
       ++thread_index) {
    workers.emplace_back([&manager, &options, thread_index]() {
      std::mt19937_64 rng(0xBADC0FFEEULL + thread_index);
      for (std::size_t op = 0; op < options.ops_per_thread; ++op) {
        const BlockId block_id = ChooseBlock(&rng, op, thread_index, options);
        auto result = manager.ReadBuffer(1, block_id);
        if (!result.ok()) {
          continue;
        }
        telepath::BufferHandle handle = std::move(result.value());
        volatile std::byte sink = handle.data()[0];
        (void)sink;
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  const auto end = std::chrono::steady_clock::now();
  const telepath::TelemetrySnapshot after = telemetry->Snapshot();
  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start)
          .count();
  const std::size_t total_ops = options.thread_count * options.ops_per_thread;
  const uint64_t hits = after.buffer_hits - before.buffer_hits;
  const uint64_t misses = after.buffer_misses - before.buffer_misses;
  const double throughput = seconds > 0.0 ? total_ops / seconds : 0.0;
  const double hit_rate =
      (hits + misses) > 0 ? static_cast<double>(hits) / (hits + misses) : 0.0;

  if (options.output_format == "csv") {
    PrintCsvSummary(options, metadata, total_ops, seconds, throughput, hits, misses,
                    hit_rate);
  } else {
    PrintTextSummary(options, metadata, total_ops, seconds, throughput, hits, misses,
                     hit_rate);
  }

  fs::remove_all(root);
  return 0;
}
