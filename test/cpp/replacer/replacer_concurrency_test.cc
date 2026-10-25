#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "telepath/replacer/replacer.h"

namespace {

constexpr std::size_t kCapacity = 64;

// Blocks a rotating subset of frames so concurrent victims see changing
// evictability, as they do inside the buffer manager.
class RotatingProbe final : public telepath::EvictionProbe {
 public:
  explicit RotatingProbe(std::size_t salt) : salt_(salt) {}
  bool IsEvictable(telepath::FrameId frame_id) const override { return (frame_id + salt_) % 5 != 0; }

 private:
  std::size_t salt_;
};

void HammerPolicy(const std::function<std::unique_ptr<telepath::Replacer>()> &make) {
  auto replacer = make();
  for (telepath::FrameId frame = 0; frame < kCapacity; ++frame) replacer->RecordLoad(frame);

  std::vector<std::thread> workers;
  for (std::size_t worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&replacer, worker]() {
      std::mt19937 rng(static_cast<unsigned>(worker + 1));
      std::uniform_int_distribution<telepath::FrameId> frame_dist(0, kCapacity - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      for (int op = 0; op < 20000; ++op) {
        const int kind = op_dist(rng);
        const telepath::FrameId frame = frame_dist(rng);
        if (kind < 80) {
          replacer->RecordAccess(frame);
        } else if (kind < 95) {
          RotatingProbe probe(static_cast<std::size_t>(op));
          telepath::FrameId victim = telepath::kInvalidFrameId;
          if (replacer->Victim(probe, &victim)) {
            assert(victim < kCapacity);
            assert(probe.IsEvictable(victim));
            replacer->RecordLoad(victim);
          }
        } else {
          replacer->Remove(frame);
          replacer->RecordLoad(frame);
        }
      }
    });
  }
  for (auto &worker : workers) worker.join();

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(RotatingProbe(1), &victim));
  assert(victim < kCapacity);
}

}  // namespace

int main() {
  HammerPolicy([] { return telepath::MakeClockReplacer(kCapacity); });
  HammerPolicy([] { return telepath::MakeClockSweepReplacer(kCapacity); });
  HammerPolicy([] { return telepath::MakeSieveReplacer(kCapacity); });
  HammerPolicy([] { return telepath::MakeLruReplacer(kCapacity); });
  HammerPolicy([] { return telepath::MakeLruKReplacer(kCapacity, 2); });
  HammerPolicy([] { return telepath::MakeTwoQueueReplacer(kCapacity); });
  return 0;
}
