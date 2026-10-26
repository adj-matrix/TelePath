// Randomized checks of every replacement policy: the Replacer contract for
// all of them, exact victims against reference models for LRU, LRU-K, and the
// plan replacer, and equivalence of a heap replacer with two replacers sharing
// one state block.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "telepath/replacer/plan_replacer.h"
#include "telepath/replacer/replacer.h"

namespace {

using telepath::FrameId;

class Probe : public telepath::EvictionProbe {
 public:
  explicit Probe(std::size_t capacity) : evictable_(capacity, true) {}
  bool IsEvictable(FrameId frame_id) const override { return frame_id < evictable_.size() && evictable_[frame_id]; }
  void Set(FrameId frame_id, bool value) { evictable_[frame_id] = value; }

 private:
  std::vector<bool> evictable_;
};

auto Victim(telepath::Replacer &replacer, const Probe &probe) -> std::optional<FrameId> {
  FrameId frame_id = telepath::kInvalidFrameId;
  if (!replacer.Victim(probe, &frame_id)) return std::nullopt;
  return frame_id;
}

// Reference policies. Each predicts the victim for the evictable frames.
class Model {
 public:
  virtual ~Model() = default;
  virtual void Load(FrameId frame_id) = 0;
  virtual void Access(FrameId frame_id) = 0;
  virtual void Remove(FrameId frame_id) = 0;
  virtual void SetNextUse(FrameId, std::uint64_t) {}
  // Frames any of which is a correct victim; empty when none is evictable.
  virtual auto Expected(const Probe &probe) const -> std::vector<FrameId> = 0;
};

class LruModel : public Model {
 public:
  void Load(FrameId frame_id) override {
    order_.remove(frame_id);
    order_.push_front(frame_id);
  }
  void Access(FrameId frame_id) override {
    if (std::find(order_.begin(), order_.end(), frame_id) != order_.end()) Load(frame_id);
  }
  void Remove(FrameId frame_id) override { order_.remove(frame_id); }
  auto Expected(const Probe &probe) const -> std::vector<FrameId> override {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
      if (probe.IsEvictable(*it)) return {*it};
    }
    return {};
  }

 private:
  std::list<FrameId> order_;
};

// Frames with fewer than K accesses go first, oldest latest access first; the
// others by oldest K-th most recent access. A load starts a new history.
class LruKModel : public Model {
 public:
  LruKModel(std::size_t capacity, std::size_t k) : k_(k), history_(capacity), tracked_(capacity, false) {}
  void Load(FrameId frame_id) override {
    tracked_[frame_id] = true;
    history_[frame_id] = {++clock_};
  }
  void Access(FrameId frame_id) override {
    if (!tracked_[frame_id]) return;
    history_[frame_id].push_back(++clock_);
    if (history_[frame_id].size() > k_) history_[frame_id].erase(history_[frame_id].begin());
  }
  void Remove(FrameId frame_id) override {
    tracked_[frame_id] = false;
    history_[frame_id].clear();
  }
  auto Expected(const Probe &probe) const -> std::vector<FrameId> override {
    std::optional<FrameId> best;
    bool best_incomplete = false;
    std::uint64_t best_key = 0;
    for (FrameId frame_id = 0; frame_id < tracked_.size(); ++frame_id) {
      if (!tracked_[frame_id] || !probe.IsEvictable(frame_id)) continue;
      const auto &history = history_[frame_id];
      const bool incomplete = history.size() < k_;
      const std::uint64_t key = incomplete ? history.back() : history.front();
      if (!best || (incomplete && !best_incomplete) || (incomplete == best_incomplete && key < best_key)) {
        best = frame_id;
        best_incomplete = incomplete;
        best_key = key;
      }
    }
    if (!best) return {};
    return {*best};
  }

 private:
  std::size_t k_;
  std::uint64_t clock_{0};
  std::vector<std::vector<std::uint64_t>> history_;
  std::vector<bool> tracked_;
};

// Evicts the frame needed furthest in the future; ties may go either way.
class PlanModel : public Model {
 public:
  explicit PlanModel(std::size_t capacity) : next_use_(capacity) {}
  void Load(FrameId frame_id) override { next_use_[frame_id] = 0; }
  void Access(FrameId) override {}
  void Remove(FrameId frame_id) override { next_use_[frame_id].reset(); }
  void SetNextUse(FrameId frame_id, std::uint64_t next_use) override {
    if (next_use_[frame_id]) next_use_[frame_id] = next_use;
  }
  auto Expected(const Probe &probe) const -> std::vector<FrameId> override {
    std::vector<FrameId> best;
    std::uint64_t best_use = 0;
    for (FrameId frame_id = 0; frame_id < next_use_.size(); ++frame_id) {
      if (!next_use_[frame_id] || !probe.IsEvictable(frame_id)) continue;
      if (best.empty() || *next_use_[frame_id] > best_use) {
        best = {frame_id};
        best_use = *next_use_[frame_id];
      } else if (*next_use_[frame_id] == best_use) {
        best.push_back(frame_id);
      }
    }
    return best;
  }

 private:
  std::vector<std::optional<std::uint64_t>> next_use_;
};

struct Policy {
  std::string name;
  std::function<std::unique_ptr<telepath::Replacer>(std::size_t)> make;
  std::function<std::unique_ptr<Model>(std::size_t)> model;
  bool plan{false};
};

auto Policies() -> std::vector<Policy> {
  return {
    {"clock", [](std::size_t c) { return telepath::MakeClockReplacer(c); }, nullptr},
    {"clock_sweep", [](std::size_t c) { return telepath::MakeClockSweepReplacer(c); }, nullptr},
    {"sieve", [](std::size_t c) { return telepath::MakeSieveReplacer(c); }, nullptr},
    {"two_queue", [](std::size_t c) { return telepath::MakeTwoQueueReplacer(c); }, nullptr},
    {"lru", [](std::size_t c) { return telepath::MakeLruReplacer(c); }, [](std::size_t) { return std::make_unique<LruModel>(); }},
    {"lru_k1", [](std::size_t c) { return telepath::MakeLruKReplacer(c, 1); }, [](std::size_t c) { return std::make_unique<LruKModel>(c, 1); }},
    {"lru_k2", [](std::size_t c) { return telepath::MakeLruKReplacer(c, 2); }, [](std::size_t c) { return std::make_unique<LruKModel>(c, 2); }},
    {"lru_k3", [](std::size_t c) { return telepath::MakeLruKReplacer(c, 3); }, [](std::size_t c) { return std::make_unique<LruKModel>(c, 3); }},
    {"plan", [](std::size_t c) { return std::make_unique<telepath::PlanReplacer>(c); }, [](std::size_t c) { return std::make_unique<PlanModel>(c); }, true},
  };
}

void Fail(const std::string &policy, std::size_t capacity, std::uint64_t seed, std::size_t step, const char *what) {
  std::fprintf(stderr, "%s capacity %zu seed %llu step %zu: %s\n", policy.c_str(), capacity, static_cast<unsigned long long>(seed), step, what);
  std::abort();
}

// Random operations, including ids outside [0, capacity) that every method
// must ignore. Victims are checked against the contract and, where there is
// one, the reference model.
void CheckContract(const Policy &policy, std::size_t capacity, std::uint64_t seed, std::size_t steps) {
  std::mt19937_64 rng(seed);
  auto pick = [&rng](std::uint64_t bound) { return std::uniform_int_distribution<std::uint64_t>(0, bound - 1)(rng); };
  auto replacer = policy.make(capacity);
  auto model = policy.model ? policy.model(capacity) : nullptr;
  auto *plan = policy.plan ? static_cast<telepath::PlanReplacer *>(replacer.get()) : nullptr;
  Probe probe(capacity);
  std::set<FrameId> tracked;

  for (std::size_t step = 0; step < steps; ++step) {
    const auto frame_id = static_cast<FrameId>(pick(capacity));
    const std::uint64_t kind = pick(100);
    if (kind < 25) {
      replacer->RecordLoad(frame_id);
      tracked.insert(frame_id);
      if (model) model->Load(frame_id);
    } else if (kind < 55) {
      replacer->RecordAccess(frame_id);
      if (model) model->Access(frame_id);
    } else if (kind < 63) {
      replacer->Remove(frame_id);
      tracked.erase(frame_id);
      if (model) model->Remove(frame_id);
    } else if (kind < 75) {
      probe.Set(frame_id, pick(4) != 0);
    } else if (kind < 78) {
      const FrameId outside = pick(2) == 0 ? static_cast<FrameId>(capacity + pick(8)) : telepath::kInvalidFrameId;
      replacer->RecordAccess(outside);
      replacer->RecordLoad(outside);
      replacer->Remove(outside);
      if (plan != nullptr) plan->SetNextUse(outside, 7);
    } else if (kind < 83 && plan != nullptr) {
      const std::uint64_t next_use = pick(4) == 0 ? telepath::PlanReplacer::kNeverUsed : pick(50);
      plan->SetNextUse(frame_id, next_use);
      model->SetNextUse(frame_id, next_use);
    } else {
      const std::optional<FrameId> victim = Victim(*replacer, probe);
      const bool any = std::any_of(tracked.begin(), tracked.end(), [&](FrameId tracked_id) { return probe.IsEvictable(tracked_id); });
      if (victim.has_value() != any) Fail(policy.name, capacity, seed, step, any ? "no victim although one is evictable" : "victim although none is evictable");
      if (!victim) continue;
      if (!tracked.contains(*victim)) Fail(policy.name, capacity, seed, step, "victim is not tracked");
      if (!probe.IsEvictable(*victim)) Fail(policy.name, capacity, seed, step, "victim is not evictable");
      if (model) {
        const std::vector<FrameId> expected = model->Expected(probe);
        if (std::find(expected.begin(), expected.end(), *victim) == expected.end()) Fail(policy.name, capacity, seed, step, "victim differs from the reference model");
      }
      // A nomination does not untrack the frame.
      const std::optional<FrameId> again = Victim(*replacer, probe);
      if (!again) Fail(policy.name, capacity, seed, step, "nominated frame stopped being tracked");
      // Nowhere to report a victim means no victim.
      if (replacer->Victim(probe, nullptr)) Fail(policy.name, capacity, seed, step, "Victim() with a null out-parameter returned true");
    }
  }
}

struct AlignedBlock {
  explicit AlignedBlock(std::size_t bytes) : data(static_cast<std::byte *>(::operator new(bytes, std::align_val_t{telepath::kCacheLineSize}))), size(bytes) {}
  ~AlignedBlock() { ::operator delete(data, std::align_val_t{telepath::kCacheLineSize}); }
  std::byte *data;
  std::size_t size;
};

// Two replacers bound to one block, used alternately, choose exactly the
// victims one heap replacer chooses for the same operations. The active
// replacer only changes right after a Victim() call, which applies its batched
// accesses, as a process does before another process takes over.
void CheckSharedEquivalence(const Policy &policy, std::size_t capacity, std::uint64_t seed, std::size_t steps) {
  std::mt19937_64 rng(seed);
  auto pick = [&rng](std::uint64_t bound) { return std::uniform_int_distribution<std::uint64_t>(0, bound - 1)(rng); };
  auto reference = policy.make(capacity);
  auto first = policy.make(capacity);
  auto second = policy.make(capacity);
  AlignedBlock block(first->SharedStateBytes());
  assert(first->BindSharedState({block.data, block.size}, true).has_value());
  assert(second->BindSharedState({block.data, block.size}, false).has_value());
  telepath::Replacer *active = first.get();
  Probe probe(capacity);

  for (std::size_t step = 0; step < steps; ++step) {
    const auto frame_id = static_cast<FrameId>(pick(capacity));
    const std::uint64_t kind = pick(100);
    if (kind < 25) {
      reference->RecordLoad(frame_id);
      active->RecordLoad(frame_id);
    } else if (kind < 60) {
      reference->RecordAccess(frame_id);
      active->RecordAccess(frame_id);
    } else if (kind < 68) {
      reference->Remove(frame_id);
      active->Remove(frame_id);
    } else if (kind < 78) {
      probe.Set(frame_id, pick(4) != 0);
    } else if (kind < 83 && policy.plan) {
      const std::uint64_t next_use = pick(50);
      static_cast<telepath::PlanReplacer *>(reference.get())->SetNextUse(frame_id, next_use);
      static_cast<telepath::PlanReplacer *>(active)->SetNextUse(frame_id, next_use);
    } else {
      if (Victim(*reference, probe) != Victim(*active, probe)) Fail(policy.name, capacity, seed, step, "shared replacers diverge from the heap replacer");
      if (pick(2) == 0) active = active == first.get() ? second.get() : first.get();
    }
  }
}

}  // namespace

int main() {
  std::uint64_t seed = 1;
  if (const char *value = std::getenv("TELEPATH_REPLACER_SEED")) seed = std::strtoull(value, nullptr, 10);
  for (const Policy &policy : Policies()) {
    for (std::size_t capacity : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}}) {
      for (std::uint64_t round = 0; round < 4; ++round) {
        CheckContract(policy, capacity, seed * 1000 + round, 4000);
        CheckSharedEquivalence(policy, capacity, seed * 1000 + round, 4000);
      }
    }
  }
  return 0;
}
