#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>

#include "replacer_test_support.h"
#include "telepath/replacer/plan_replacer.h"
#include "telepath/replacer/replacer.h"

namespace {

using telepath::replacer_test_support::NextVictim;
using telepath::replacer_test_support::SetProbe;

constexpr std::size_t kCapacity = 16;

struct AlignedBlock {
  explicit AlignedBlock(std::size_t bytes)
    : data(static_cast<std::byte *>(::operator new(bytes + telepath::kCacheLineSize, std::align_val_t{telepath::kCacheLineSize}))), size(bytes) {}
  ~AlignedBlock() { ::operator delete(data, std::align_val_t{telepath::kCacheLineSize}); }

  auto span() const -> std::span<std::byte> { return {data, size}; }

  std::byte *data;
  std::size_t size;
};

using Factory = std::function<std::unique_ptr<telepath::Replacer>()>;

// Two replacers bound to one block behave as one policy, the way two processes
// sharing a segment do.
void ExpectSharedStateIsVisibleThroughBothReplacers(const std::string &name, const Factory &make) {
  auto first = make();
  auto second = make();
  const std::size_t bytes = first->SharedStateBytes();
  assert(bytes > 0 && bytes == second->SharedStateBytes());

  AlignedBlock block(bytes);
  assert(first->BindSharedState(block.span(), true).has_value());
  assert(second->BindSharedState(block.span(), false).has_value());

  for (telepath::FrameId frame = 0; frame < 4; ++frame) first->RecordLoad(frame);
  const telepath::FrameId victim = NextVictim(second.get(), SetProbe{});
  assert(victim < 4);

  second->Remove(victim);
  for (int round = 0; round < 8; ++round) {
    const telepath::FrameId again = NextVictim(first.get(), SetProbe{});
    assert(again != victim && again < 4);
  }
  first->Remove(0);
  first->Remove(1);
  first->Remove(2);
  first->Remove(3);
  assert(NextVictim(second.get(), SetProbe{}) == telepath::kInvalidFrameId);
  (void)name;
}

void ExpectBadMemoryIsRejected() {
  auto replacer = telepath::MakeClockSweepReplacer(kCapacity);
  AlignedBlock block(replacer->SharedStateBytes() + telepath::kCacheLineSize);
  auto too_small = replacer->BindSharedState(std::span<std::byte>(block.data, replacer->SharedStateBytes() - 1), true);
  assert(!too_small.has_value() && too_small.error().code() == telepath::ErrorCode::kInvalidArgument);
  auto misaligned = replacer->BindSharedState(std::span<std::byte>(block.data + 8, replacer->SharedStateBytes()), true);
  assert(!misaligned.has_value() && misaligned.error().code() == telepath::ErrorCode::kInvalidArgument);
}

class HeapOnlyReplacer final : public telepath::Replacer {
 public:
  void RecordAccess(telepath::FrameId) override {}
  void RecordLoad(telepath::FrameId) override {}
  bool Victim(const telepath::EvictionProbe &, telepath::FrameId *) override { return false; }
  void Remove(telepath::FrameId) override {}
};

void ExpectCustomReplacerDefaultsToHeapOnly() {
  HeapOnlyReplacer replacer;
  assert(replacer.SharedStateBytes() == 0);
  AlignedBlock block(64);
  auto refused = replacer.BindSharedState(block.span(), true);
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kUnavailable);
}

}  // namespace

int main() {
  ExpectSharedStateIsVisibleThroughBothReplacers("clock", [] { return telepath::MakeClockReplacer(kCapacity); });
  ExpectSharedStateIsVisibleThroughBothReplacers("clock_sweep", [] { return telepath::MakeClockSweepReplacer(kCapacity); });
  ExpectSharedStateIsVisibleThroughBothReplacers("sieve", [] { return telepath::MakeSieveReplacer(kCapacity); });
  ExpectSharedStateIsVisibleThroughBothReplacers("lru", [] { return telepath::MakeLruReplacer(kCapacity); });
  ExpectSharedStateIsVisibleThroughBothReplacers("lru_k", [] { return telepath::MakeLruKReplacer(kCapacity, 2); });
  ExpectSharedStateIsVisibleThroughBothReplacers("two_queue", [] { return telepath::MakeTwoQueueReplacer(kCapacity); });
  ExpectSharedStateIsVisibleThroughBothReplacers("plan", [] { return std::make_unique<telepath::PlanReplacer>(kCapacity); });
  ExpectBadMemoryIsRejected();
  ExpectCustomReplacerDefaultsToHeapOnly();
  return 0;
}
