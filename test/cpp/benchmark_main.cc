#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

struct BenchmarkOptions {
  std::string workload{"hotspot"};
  std::string output_format{"text"};
  std::size_t pool_size{256};
  std::size_t page_size{4096};
  std::size_t block_count{1024};
  std::size_t thread_count{4};
  std::size_t ops_per_thread{5000};
  std::size_t hotset_size{64};
  std::size_t hot_access_percent{80};
};

std::size_t ParsePositive(const char *value, std::size_t fallback) {
  if (value == nullptr) {
    return fallback;
  }
  try {
    const unsigned long long parsed = std::stoull(value);
    return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
  } catch (...) {
    return fallback;
  }
}

BenchmarkOptions ParseArgs(int argc, char **argv) {
  BenchmarkOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--workload" && i + 1 < argc) {
      options.workload = argv[++i];
    } else if (arg == "--output-format" && i + 1 < argc) {
      options.output_format = argv[++i];
    } else if (arg == "--pool-size" && i + 1 < argc) {
      options.pool_size = ParsePositive(argv[++i], options.pool_size);
    } else if (arg == "--block-count" && i + 1 < argc) {
      options.block_count = ParsePositive(argv[++i], options.block_count);
    } else if (arg == "--threads" && i + 1 < argc) {
      options.thread_count = ParsePositive(argv[++i], options.thread_count);
    } else if (arg == "--ops-per-thread" && i + 1 < argc) {
      options.ops_per_thread = ParsePositive(argv[++i], options.ops_per_thread);
    } else if (arg == "--hotset-size" && i + 1 < argc) {
      options.hotset_size = ParsePositive(argv[++i], options.hotset_size);
    } else if (arg == "--hot-access-percent" && i + 1 < argc) {
      options.hot_access_percent =
          ParsePositive(argv[++i], options.hot_access_percent);
      if (options.hot_access_percent > 100) {
        options.hot_access_percent = 100;
      }
    }
  }
  if (options.hotset_size > options.block_count) {
    options.hotset_size = options.block_count;
  }
  return options;
}

telepath::BlockId ChooseBlock(std::mt19937_64 *rng,
                              std::size_t op_index,
                              const BenchmarkOptions &options) {
  if (options.workload == "uniform") {
    std::uniform_int_distribution<std::size_t> dist(0, options.block_count - 1);
    return static_cast<telepath::BlockId>(dist(*rng));
  }

  if (options.workload == "sequential") {
    return static_cast<telepath::BlockId>(op_index % options.block_count);
  }

  std::uniform_int_distribution<std::size_t> percent_dist(0, 99);
  const bool choose_hot = percent_dist(*rng) < options.hot_access_percent;

  if (choose_hot && options.hotset_size > 0) {
    std::uniform_int_distribution<std::size_t> hot_dist(0, options.hotset_size - 1);
    return static_cast<telepath::BlockId>(hot_dist(*rng));
  }

  std::uniform_int_distribution<std::size_t> cold_dist(0, options.block_count - 1);
  return static_cast<telepath::BlockId>(cold_dist(*rng));
}

void PrintTextSummary(const BenchmarkOptions &options, std::size_t total_ops,
                      double seconds, double throughput, uint64_t hits,
                      uint64_t misses, double hit_rate) {
  std::cout << "telepath_benchmark\n";
  std::cout << "workload=" << options.workload << "\n";
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

void PrintCsvSummary(const BenchmarkOptions &options, std::size_t total_ops,
                     double seconds, double throughput, uint64_t hits,
                     uint64_t misses, double hit_rate) {
  std::cout
      << "workload,threads,pool_size,block_count,ops_per_thread,hotset_size,"
         "hot_access_percent,total_ops,seconds,throughput_ops_per_sec,"
         "buffer_hits,buffer_misses,hit_rate\n";
  std::cout << options.workload << ","
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
        const BlockId block_id = ChooseBlock(&rng, op, options);
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
    PrintCsvSummary(options, total_ops, seconds, throughput, hits, misses,
                    hit_rate);
  } else {
    PrintTextSummary(options, total_ops, seconds, throughput, hits, misses,
                     hit_rate);
  }

  fs::remove_all(root);
  return 0;
}
