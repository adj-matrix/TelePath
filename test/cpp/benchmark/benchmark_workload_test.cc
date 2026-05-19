#include <cassert>
#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

#include "benchmark_support.h"

namespace {

void ExpectNormalizedSequentialWorkload() {
  const auto normalized = telepath::benchmark_support::NormalizeWorkloadName("sequential");
  assert(normalized == "sequential_disjoint");
}

void ExpectSequentialSharedUsesSameBlockAcrossThreads() {
  telepath::benchmark_support::BenchmarkOptions options;
  options.workload = "sequential_shared";
  options.block_count = 32;
  options.thread_count = 4;
  std::mt19937_64 rng(7);

  for (std::size_t op = 0; op < 16; ++op) {
    const auto first = telepath::benchmark_support::ChooseBlock(&rng, op, 0, options);
    const auto second = telepath::benchmark_support::ChooseBlock(&rng, op, 3, options);
    assert(first == second);
  }
}

void ExpectSequentialDisjointUsesDistinctBlocks() {
  telepath::benchmark_support::BenchmarkOptions options;
  options.workload = "sequential_disjoint";
  options.block_count = 64;
  options.thread_count = 4;
  std::mt19937_64 rng(11);

  for (std::size_t op = 0; op < 16; ++op) {
    std::unordered_set<telepath::BlockId> blocks;
    for (std::size_t thread = 0; thread < options.thread_count; ++thread) {
      blocks.insert(telepath::benchmark_support::ChooseBlock(&rng, op, thread, options));
    }
    assert(blocks.size() == options.thread_count);
  }
}

void ExpectHotspotUsesHotsetWhenConfiguredForOneHundredPercent() {
  telepath::benchmark_support::BenchmarkOptions options;
  options.workload = "hotspot";
  options.block_count = 128;
  options.hotset_size = 8;
  options.hot_access_percent = 100;
  std::mt19937_64 rng(19);

  for (std::size_t op = 0; op < 256; ++op) {
    const auto block = telepath::benchmark_support::ChooseBlock(&rng, op, 0, options);
    assert(block < options.hotset_size);
  }
}

void ExpectDirtyAliasNormalizesToHotspotWithWrites() {
  const char *argv[] = {
    "telepath_benchmark",
    "--workload",
    "dirty",
    "--write-percent",
    "100",
    "--flush-every-ops",
    "8",
    "--background-cleaner",
    "true",
    "--replacer",
    "2q",
    "--disk-backend",
    "uring",
    "--telemetry-shm-name",
    "/telepath_benchmark_workload_test",
    "--telemetry-shm-capacity",
    "32768",
    "--snapshot-sample-limit",
    "6",
  };
  auto options = telepath::benchmark_support::ParseArgs(
    static_cast<int>(sizeof(argv) / sizeof(argv[0])),
    const_cast<char **>(argv));
  assert(options.workload == "hotspot");
  assert(options.write_percent == 100);
  assert(options.flush_every_ops == 8);
  assert(options.enable_background_cleaner);
  assert(options.replacer == "two_queue");
  assert(options.disk_backend == "io_uring");
  assert(options.telemetry_shm_name == "/telepath_benchmark_workload_test");
  assert(options.telemetry_shm_capacity == 32768);
  assert(options.snapshot_sample_limit == 6);
}

void ExpectInvalidExperimentKnobsFallBackToDefaults() {
  const char *argv[] = {
    "telepath_benchmark",
    "--replacer",
    "not-a-replacer",
    "--disk-backend",
    "not-a-backend",
    "--write-percent",
    "125",
    "--hot-access-percent",
    "-1",
  };
  auto options = telepath::benchmark_support::ParseArgs(
    static_cast<int>(sizeof(argv) / sizeof(argv[0])),
    const_cast<char **>(argv));
  assert(options.replacer == "lru_k");
  assert(options.disk_backend == "auto");
  assert(options.write_percent == 100);
  assert(options.hot_access_percent == 80);
}

void ExpectWriteOperationHonorsZeroAndHundredPercent() {
  telepath::benchmark_support::BenchmarkOptions options;
  std::mt19937_64 rng(23);

  options.write_percent = 0;
  assert(!telepath::benchmark_support::ShouldWriteOperation(&rng, 0, options));

  options.write_percent = 100;
  assert(telepath::benchmark_support::ShouldWriteOperation(&rng, 0, options));
}

void ExpectLatencySummaryUsesNearestRankPercentiles() {
  auto empty = telepath::benchmark_support::SummarizeLatencySamples({});
  assert(empty.min_ns == 0);
  assert(empty.p50_ns == 0);
  assert(empty.p95_ns == 0);
  assert(empty.p99_ns == 0);
  assert(empty.max_ns == 0);
  assert(empty.average_ns == 0.0);

  auto summary = telepath::benchmark_support::SummarizeLatencySamples(std::vector<uint64_t>{50, 10, 40, 20, 30});
  assert(summary.min_ns == 10);
  assert(summary.p50_ns == 30);
  assert(summary.p95_ns == 50);
  assert(summary.p99_ns == 50);
  assert(summary.max_ns == 50);
  assert(summary.average_ns == 30.0);
}

}  // namespace

int main() {
  ExpectNormalizedSequentialWorkload();
  ExpectSequentialSharedUsesSameBlockAcrossThreads();
  ExpectSequentialDisjointUsesDistinctBlocks();
  ExpectHotspotUsesHotsetWhenConfiguredForOneHundredPercent();
  ExpectDirtyAliasNormalizesToHotspotWithWrites();
  ExpectInvalidExperimentKnobsFallBackToDefaults();
  ExpectWriteOperationHonorsZeroAndHundredPercent();
  ExpectLatencySummaryUsesNearestRankPercentiles();

  return 0;
}
