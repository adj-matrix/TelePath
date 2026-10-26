#include <cassert>
#include <cstdint>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "indexed_ordered_set.h"

namespace {

void ExpectMatchesStdSetUnderRandomOperations() {
  constexpr std::size_t kCapacity = 257;
  std::vector<telepath::IndexedOrderedSet::Node> nodes(kCapacity);
  telepath::FrameId root = telepath::IndexedOrderedSet::kNone;
  telepath::IndexedOrderedSet set(nodes.data(), &root);
  set.Reset(kCapacity);

  std::set<std::pair<std::uint64_t, telepath::FrameId>> reference;
  std::vector<std::uint64_t> keys(kCapacity, 0);
  std::mt19937_64 rng(42);
  for (int step = 0; step < 20000; ++step) {
    const auto frame = static_cast<telepath::FrameId>(rng() % kCapacity);
    if (set.Contains(frame)) {
      set.Erase(frame);
      reference.erase({keys[frame], frame});
    }
    if (rng() % 3 != 0) {
      keys[frame] = rng() % 64;
      set.Insert(frame, keys[frame]);
      reference.insert({keys[frame], frame});
    }
    if (step % 97 != 0) continue;
    auto it = reference.begin();
    for (telepath::FrameId current = set.First(); current != telepath::IndexedOrderedSet::kNone; current = set.Next(current), ++it) {
      assert(it != reference.end() && it->second == current);
    }
    assert(it == reference.end());
    auto rit = reference.rbegin();
    for (telepath::FrameId current = set.Last(); current != telepath::IndexedOrderedSet::kNone; current = set.Previous(current), ++rit) {
      assert(rit != reference.rend() && rit->second == current);
    }
    assert(rit == reference.rend());
  }
}

}  // namespace

int main() {
  ExpectMatchesStdSetUnderRandomOperations();
  return 0;
}
