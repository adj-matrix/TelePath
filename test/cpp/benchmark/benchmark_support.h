#ifndef TELEPATH_TEST_CPP_BENCHMARK_SUPPORT_H_
#define TELEPATH_TEST_CPP_BENCHMARK_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <random>
#include <string>

#include "telepath/common/types.h"

namespace telepath::benchmark_support {

inline auto NormalizeWorkloadName(const std::string &workload) -> std::string {
  if (workload == "sequential") return "sequential_disjoint";
  if (workload == "dirty") return "hotspot";
  return workload;
}

struct BenchmarkOptions {
  std::string workload{"hotspot"};
  std::string output_format{"text"};
  std::string replacer{"lru_k"};
  std::string disk_backend{"auto"};
  std::string telemetry_export_path{};
  std::size_t pool_size{256};
  std::size_t page_size{4096};
  std::size_t block_count{1024};
  std::size_t thread_count{4};
  std::size_t ops_per_thread{5000};
  std::size_t hotset_size{64};
  std::size_t hot_access_percent{80};
  std::size_t write_percent{0};
  std::size_t flush_every_ops{0};
  std::size_t flush_worker_count{0};
  std::size_t flush_submit_batch_size{0};
  std::size_t flush_foreground_burst_limit{0};
  bool enable_background_cleaner{false};
  std::size_t dirty_page_high_watermark{0};
  std::size_t dirty_page_low_watermark{0};
  std::size_t queue_depth{0};
  std::size_t max_open_files{0};
};

struct BenchmarkMetadata {
  std::string commit_sha{"local"};
  std::string runner_os{"local"};
  std::string runner_arch{"unknown"};
};

inline auto ParseNonNegative(const char *value, std::size_t fallback) -> std::size_t {
  if (value == nullptr) return fallback;
  if (value[0] == '-') return fallback;
  try {
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (value[consumed] != '\0') return fallback;
    return static_cast<std::size_t>(parsed);
  } catch (...) {
    return fallback;
  }
}

inline auto ParsePositive(const char *value, std::size_t fallback) -> std::size_t {
  const std::size_t parsed = ParseNonNegative(value, fallback);
  return parsed == 0 ? fallback : parsed;
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

inline bool TryParseNonNegativeArg(
  const std::string &arg,
  const char *name,
  int argc,
  char **argv,
  int *index,
  std::size_t *value
) {
  if (arg != name) return false;
  if (*index + 1 >= argc) return true;
  *value = ParseNonNegative(argv[++(*index)], *value);
  return true;
}

inline void ClampHotAccessPercent(BenchmarkOptions *options) {
  if (options->hot_access_percent > 100) options->hot_access_percent = 100;
}

inline void ClampWritePercent(BenchmarkOptions *options) {
  if (options->write_percent > 100) options->write_percent = 100;
}

inline void ClampHotsetSize(BenchmarkOptions *options) {
  if (options->hotset_size > options->block_count) options->hotset_size = options->block_count;
}

inline auto ParseBoolValue(const char *value, bool fallback) -> bool {
  if (value == nullptr) return fallback;
  std::string parsed = value;
  std::transform(parsed.begin(), parsed.end(), parsed.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (parsed == "1" || parsed == "true" || parsed == "yes" || parsed == "on") return true;
  if (parsed == "0" || parsed == "false" || parsed == "no" || parsed == "off") return false;
  return fallback;
}

inline bool TryParseBoolArg(
  const std::string &arg,
  const char *name,
  int argc,
  char **argv,
  int *index,
  bool *value
) {
  if (arg != name) return false;
  if (*index + 1 >= argc) return true;
  *value = ParseBoolValue(argv[++(*index)], *value);
  return true;
}

inline void NormalizeBenchmarkOptions(BenchmarkOptions *options) {
  options->workload = NormalizeWorkloadName(options->workload);
  if (options->replacer == "lruk") options->replacer = "lru_k";
  if (options->replacer == "2q") options->replacer = "two_queue";
  if (
    options->replacer != "clock" &&
    options->replacer != "lru" &&
    options->replacer != "lru_k" &&
    options->replacer != "two_queue"
  ) {
    options->replacer = "lru_k";
  }
  if (options->disk_backend == "uring") options->disk_backend = "io_uring";
  if (
    options->disk_backend != "auto" &&
    options->disk_backend != "posix" &&
    options->disk_backend != "io_uring"
  ) {
    options->disk_backend = "auto";
  }
  ClampHotAccessPercent(options);
  ClampWritePercent(options);
  ClampHotsetSize(options);
}

inline auto ParseArgs(int argc, char **argv) -> BenchmarkOptions {
  BenchmarkOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (TryParseStringArg(arg, "--workload", argc, argv, &i, &options.workload)) continue;
    if (TryParseStringArg(arg, "--output-format", argc, argv, &i, &options.output_format)) continue;
    if (TryParseStringArg(arg, "--replacer", argc, argv, &i, &options.replacer)) continue;
    if (TryParseStringArg(arg, "--disk-backend", argc, argv, &i, &options.disk_backend)) continue;
    if (TryParseStringArg(arg, "--telemetry-export-path", argc, argv, &i, &options.telemetry_export_path)) continue;
    if (TryParsePositiveArg(arg, "--pool-size", argc, argv, &i, &options.pool_size)) continue;
    if (TryParsePositiveArg(arg, "--block-count", argc, argv, &i, &options.block_count)) continue;
    if (TryParsePositiveArg(arg, "--threads", argc, argv, &i, &options.thread_count)) continue;
    if (TryParsePositiveArg(arg, "--ops-per-thread", argc, argv, &i, &options.ops_per_thread)) continue;
    if (TryParsePositiveArg(arg, "--hotset-size", argc, argv, &i, &options.hotset_size)) continue;
    if (TryParseNonNegativeArg(arg, "--hot-access-percent", argc, argv, &i, &options.hot_access_percent)) continue;
    if (TryParseNonNegativeArg(arg, "--write-percent", argc, argv, &i, &options.write_percent)) continue;
    if (TryParseNonNegativeArg(arg, "--flush-every-ops", argc, argv, &i, &options.flush_every_ops)) continue;
    if (TryParseNonNegativeArg(arg, "--flush-workers", argc, argv, &i, &options.flush_worker_count)) continue;
    if (TryParseNonNegativeArg(arg, "--flush-submit-batch-size", argc, argv, &i, &options.flush_submit_batch_size)) continue;
    if (TryParseNonNegativeArg(arg, "--flush-foreground-burst-limit", argc, argv, &i, &options.flush_foreground_burst_limit)) continue;
    if (TryParseBoolArg(arg, "--background-cleaner", argc, argv, &i, &options.enable_background_cleaner)) continue;
    if (TryParseNonNegativeArg(arg, "--dirty-page-high-watermark", argc, argv, &i, &options.dirty_page_high_watermark)) continue;
    if (TryParseNonNegativeArg(arg, "--dirty-page-low-watermark", argc, argv, &i, &options.dirty_page_low_watermark)) continue;
    if (TryParseNonNegativeArg(arg, "--queue-depth", argc, argv, &i, &options.queue_depth)) continue;
    if (!TryParseNonNegativeArg(arg, "--max-open-files", argc, argv, &i, &options.max_open_files)) continue;
  }
  NormalizeBenchmarkOptions(&options);
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

inline bool ShouldWriteOperation(
  std::mt19937_64 *rng,
  std::size_t op_index,
  const BenchmarkOptions &options
) {
  if (options.write_percent == 0) return false;
  if (options.flush_every_ops != 0 && (op_index + 1) % options.flush_every_ops == 0) return true;
  if (options.write_percent >= 100) return true;

  std::uniform_int_distribution<std::size_t> percent_dist(0, 99);
  return percent_dist(*rng) < options.write_percent;
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
