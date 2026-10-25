#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_access_strategy.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr std::size_t kPoolSize = 16;
constexpr telepath::BlockId kHotPages = 8;
constexpr telepath::BlockId kColdStart = 100;
constexpr telepath::BlockId kScanLength = 200;

using ReplacerFactory = std::function<std::unique_ptr<telepath::Replacer>()>;

auto CreateManager(const std::filesystem::path &root, const ReplacerFactory &make_replacer) -> std::unique_ptr<telepath::BufferManager> {
  auto manager = telepath::BufferManager::Create(
    kPoolSize, 4096,
    std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>(), false, 64, false),
    make_replacer(), telepath::MakeNoOpTelemetrySink());
  assert(manager.has_value());
  return std::move(manager.value());
}

void WarmHotSet(telepath::BufferManager *manager) {
  for (int round = 0; round < 4; ++round) {
    for (telepath::BlockId block = 0; block < kHotPages; ++block) assert(manager->ReadBuffer(1, block).has_value());
  }
}

void Scan(telepath::BufferManager *manager, telepath::BufferAccessStrategy *strategy) {
  for (telepath::BlockId block = kColdStart; block < kColdStart + kScanLength; ++block) {
    assert(manager->ReadBuffer(1, block, strategy).has_value());
  }
}

auto CountResident(const telepath::BufferManager &manager, telepath::BlockId first, telepath::BlockId last) -> std::size_t {
  std::size_t count = 0;
  for (const auto &frame : manager.ExportSnapshot().frames) {
    if (frame.is_valid && frame.tag.block_id >= first && frame.tag.block_id < last) ++count;
  }
  return count;
}

// Plain LRU is the policy a scan pollutes; clock-sweep, SIEVE, and LRU-K
// resist scans on their own, so only the strategy's ring bound is checked
// for them.
void ExpectPlainLruScanEvictsHotSet() {
  telepath::buffer_test_support::TestRootGuard root("telepath_access_strategy_plain_lru");
  auto plain = CreateManager(root.path(), [] { return telepath::MakeLruReplacer(kPoolSize); });
  WarmHotSet(plain.get());
  Scan(plain.get(), nullptr);
  assert(CountResident(*plain, 0, kHotPages) < kHotPages);
}

void ExpectBulkReadScanKeepsHotSetResident(const std::string &name, const ReplacerFactory &make_replacer) {
  telepath::buffer_test_support::TestRootGuard root("telepath_access_strategy_" + name);

  auto guarded = CreateManager(root.path(), make_replacer);
  WarmHotSet(guarded.get());
  telepath::BufferAccessStrategy strategy(telepath::BufferAccessStrategyType::kBulkRead, 4);
  assert(strategy.ring_size() == 4);
  Scan(guarded.get(), &strategy);
  assert(CountResident(*guarded, 0, kHotPages) == kHotPages);
  assert(CountResident(*guarded, kColdStart, kColdStart + kScanLength) <= strategy.ring_size());
}

void ExpectNormalStrategyBehavesLikeNoStrategy() {
  telepath::BufferAccessStrategy strategy(telepath::BufferAccessStrategyType::kNormal);
  assert(strategy.ring_size() == 0);
  telepath::buffer_test_support::TestRootGuard root("telepath_access_strategy_normal");
  auto manager = CreateManager(root.path(), [] { return telepath::MakeLruReplacer(kPoolSize); });
  WarmHotSet(manager.get());
  Scan(manager.get(), &strategy);
  assert(CountResident(*manager, 0, kHotPages) < kHotPages);
}

}  // namespace

int main() {
  ExpectPlainLruScanEvictsHotSet();
  ExpectBulkReadScanKeepsHotSetResident("clock_sweep", [] { return telepath::MakeClockSweepReplacer(kPoolSize); });
  ExpectBulkReadScanKeepsHotSetResident("sieve", [] { return telepath::MakeSieveReplacer(kPoolSize); });
  ExpectBulkReadScanKeepsHotSetResident("lru", [] { return telepath::MakeLruReplacer(kPoolSize); });
  ExpectBulkReadScanKeepsHotSetResident("lru_k", [] { return telepath::MakeLruKReplacer(kPoolSize, 2); });
  ExpectNormalStrategyBehavesLikeNoStrategy();
  return 0;
}
