#include <cassert>
#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_set>

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

}  // namespace

int main() {
  ExpectNormalizedSequentialWorkload();
  ExpectSequentialSharedUsesSameBlockAcrossThreads();
  ExpectSequentialDisjointUsesDistinctBlocks();
  ExpectHotspotUsesHotsetWhenConfiguredForOneHundredPercent();

  return 0;
}
