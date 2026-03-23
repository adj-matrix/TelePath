#ifndef TELEPATH_TEST_CPP_BENCHMARK_SUPPORT_H_
#define TELEPATH_TEST_CPP_BENCHMARK_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>

#include "telepath/common/types.h"

namespace telepath::benchmark_support {

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

struct BenchmarkMetadata {
  std::string commit_sha{"local"};
  std::string runner_os{"local"};
  std::string runner_arch{"unknown"};
};

inline std::size_t ParsePositive(const char *value, std::size_t fallback) {
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

inline BenchmarkOptions ParseArgs(int argc, char **argv) {
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

inline BenchmarkMetadata LoadMetadata() {
  BenchmarkMetadata metadata;
  if (const char *value = std::getenv("GITHUB_SHA"); value != nullptr &&
                                                     value[0] != '\0') {
    metadata.commit_sha = value;
  }
  if (const char *value = std::getenv("RUNNER_OS"); value != nullptr &&
                                                    value[0] != '\0') {
    metadata.runner_os = value;
  }
  if (const char *value = std::getenv("RUNNER_ARCH"); value != nullptr &&
                                                      value[0] != '\0') {
    metadata.runner_arch = value;
  }
  return metadata;
}

inline std::string NormalizeWorkloadName(const std::string &workload) {
  if (workload == "sequential") {
    return "sequential_disjoint";
  }
  return workload;
}

inline BlockId ChooseBlock(std::mt19937_64 *rng, std::size_t op_index,
                           std::size_t thread_index,
                           const BenchmarkOptions &options) {
  const std::string workload = NormalizeWorkloadName(options.workload);
  if (workload == "uniform") {
    std::uniform_int_distribution<std::size_t> dist(0, options.block_count - 1);
    return static_cast<BlockId>(dist(*rng));
  }

  if (workload == "sequential_shared") {
    return static_cast<BlockId>(op_index % options.block_count);
  }

  if (workload == "sequential_disjoint") {
    return static_cast<BlockId>(
        ((op_index * options.thread_count) + thread_index) % options.block_count);
  }

  std::uniform_int_distribution<std::size_t> percent_dist(0, 99);
  const bool choose_hot = percent_dist(*rng) < options.hot_access_percent;

  if (choose_hot && options.hotset_size > 0) {
    std::uniform_int_distribution<std::size_t> hot_dist(0,
                                                        options.hotset_size - 1);
    return static_cast<BlockId>(hot_dist(*rng));
  }

  std::uniform_int_distribution<std::size_t> cold_dist(0, options.block_count - 1);
  return static_cast<BlockId>(cold_dist(*rng));
}

}  // namespace telepath::benchmark_support

#endif  // TELEPATH_TEST_CPP_BENCHMARK_SUPPORT_H_
