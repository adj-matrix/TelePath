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

inline auto ParsePositive(const char *value, std::size_t fallback) -> std::size_t {
  if (value == nullptr) return fallback;
  try {
    const unsigned long long parsed = std::stoull(value);
    return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
  } catch (...) {
    return fallback;
  }
}

inline bool TryParseStringArg(
  const std::string &arg,
  const char *name,
  int argc,
  char **argv,
  int *index,
  std::string *value
) {
  if (arg != name) return false;
  if (*index + 1 >= argc) return true;
  *value = argv[++(*index)];
  return true;
}

inline bool TryParsePositiveArg(
  const std::string &arg,
  const char *name,
  int argc,
  char **argv,
  int *index,
  std::size_t *value
) {
  if (arg != name) return false;
  if (*index + 1 >= argc) return true;
  *value = ParsePositive(argv[++(*index)], *value);
  return true;
}

inline void ClampHotAccessPercent(BenchmarkOptions *options) {
  if (options->hot_access_percent > 100) options->hot_access_percent = 100;
}

inline void ClampHotsetSize(BenchmarkOptions *options) {
  if (options->hotset_size > options->block_count) options->hotset_size = options->block_count;
}

inline auto ParseArgs(int argc, char **argv) -> BenchmarkOptions {
  BenchmarkOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (TryParseStringArg(arg, "--workload", argc, argv, &i, &options.workload)) continue;
    if (TryParseStringArg(arg, "--output-format", argc, argv, &i, &options.output_format)) continue;
    if (TryParsePositiveArg(arg, "--pool-size", argc, argv, &i, &options.pool_size)) continue;
    if (TryParsePositiveArg(arg, "--block-count", argc, argv, &i, &options.block_count)) continue;
    if (TryParsePositiveArg(arg, "--threads", argc, argv, &i, &options.thread_count)) continue;
    if (TryParsePositiveArg(arg, "--ops-per-thread", argc, argv, &i, &options.ops_per_thread)) continue;
    if (TryParsePositiveArg(arg, "--hotset-size", argc, argv, &i, &options.hotset_size)) continue;
    if (!TryParsePositiveArg(arg, "--hot-access-percent", argc, argv, &i, &options.hot_access_percent)) continue;
    ClampHotAccessPercent(&options);
  }
  ClampHotsetSize(&options);
  return options;
}

inline void LoadMetadataFromEnv(
  const char *env_name,
  std::string *target
) {
  const char *value = std::getenv(env_name);
  if (value == nullptr || value[0] == '\0') return;
  *target = value;
}

inline auto LoadMetadata() -> BenchmarkMetadata {
  BenchmarkMetadata metadata;
  LoadMetadataFromEnv("GITHUB_SHA", &metadata.commit_sha);
  LoadMetadataFromEnv("RUNNER_OS", &metadata.runner_os);
  LoadMetadataFromEnv("RUNNER_ARCH", &metadata.runner_arch);
  return metadata;
}

inline auto NormalizeWorkloadName(const std::string &workload) -> std::string {
  if (workload == "sequential") return "sequential_disjoint";
  return workload;
}

inline auto ChooseBlock(
  std::mt19937_64 *rng,
  std::size_t op_index,
  std::size_t thread_index,
  const BenchmarkOptions &options
) -> BlockId {
  const std::string workload = NormalizeWorkloadName(options.workload);
  if (workload == "uniform") {
    std::uniform_int_distribution<std::size_t> dist(0, options.block_count - 1);
    return static_cast<BlockId>(dist(*rng));
  }

  if (workload == "sequential_shared") return static_cast<BlockId>(op_index % options.block_count);

  if (workload == "sequential_disjoint") return static_cast<BlockId>(((op_index * options.thread_count) + thread_index) % options.block_count);

  std::uniform_int_distribution<std::size_t> percent_dist(0, 99);
  const bool choose_hot = percent_dist(*rng) < options.hot_access_percent;

  if (choose_hot && options.hotset_size > 0) {
    std::uniform_int_distribution<std::size_t> hot_dist(0, options.hotset_size - 1);
    return static_cast<BlockId>(hot_dist(*rng));
  }

  std::uniform_int_distribution<std::size_t> cold_dist(0, options.block_count - 1);
  return static_cast<BlockId>(cold_dist(*rng));
}

}  // namespace telepath::benchmark_support

#endif  // TELEPATH_TEST_CPP_BENCHMARK_SUPPORT_H_
