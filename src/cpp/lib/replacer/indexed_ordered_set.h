#ifndef TELEPATH_LIB_REPLACER_INDEXED_ORDERED_SET_H_
#define TELEPATH_LIB_REPLACER_INDEXED_ORDERED_SET_H_

#include <cstddef>
#include <cstdint>

#include "telepath/common/types.h"

namespace telepath {

// An ordered set of frame ids keyed by (key, frame id): a treap whose nodes are
// the frames themselves and whose links are indices, so it needs no
// allocation and can live in shared memory. The arrays and the root belong to
// the caller; Node arrays must start zeroed except for `kNone` links, which
// Reset() establishes. Not thread-safe.
class IndexedOrderedSet {
 public:
  static constexpr FrameId kNone = kInvalidFrameId;

  struct Node {
    std::uint64_t key{0};
    FrameId left{kNone};
    FrameId right{kNone};
    FrameId parent{kNone};
    bool linked{false};
  };

  IndexedOrderedSet() = default;
  IndexedOrderedSet(Node *nodes, FrameId *root) : nodes_(nodes), root_(root) {}

  void Reset(std::size_t capacity) {
    *root_ = kNone;
    for (std::size_t i = 0; i < capacity; ++i) nodes_[i] = Node{};
  }

  bool Contains(FrameId frame) const { return nodes_[frame].linked; }

  void Insert(FrameId frame, std::uint64_t key) {
    Node &node = nodes_[frame];
    node = Node{key, kNone, kNone, kNone, true};
    FrameId parent = kNone;
    FrameId *link = root_;
    while (*link != kNone) {
      parent = *link;
      link = Less(frame, parent) ? &nodes_[parent].left : &nodes_[parent].right;
    }
    *link = frame;
    node.parent = parent;
    while (node.parent != kNone && Priority(frame) > Priority(node.parent)) Rotate(frame);
  }

  void Erase(FrameId frame) {
    Node &node = nodes_[frame];
    if (!node.linked) return;
    while (node.left != kNone || node.right != kNone) {
      const FrameId child = node.left == kNone ? node.right
        : node.right == kNone ? node.left
        : Priority(node.left) > Priority(node.right) ? node.left : node.right;
      Rotate(child);
    }
    Replace(frame, kNone);
    node.linked = false;
  }

  auto First() const -> FrameId { return Extreme(*root_, true); }
  auto Last() const -> FrameId { return Extreme(*root_, false); }
  auto Next(FrameId frame) const -> FrameId { return Step(frame, true); }
  auto Previous(FrameId frame) const -> FrameId { return Step(frame, false); }

 private:
  static auto Priority(FrameId frame) -> std::uint64_t {
    std::uint64_t value = frame + 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

  bool Less(FrameId a, FrameId b) const {
    return nodes_[a].key < nodes_[b].key || (nodes_[a].key == nodes_[b].key && a < b);
  }

  auto Extreme(FrameId frame, bool leftmost) const -> FrameId {
    if (frame == kNone) return kNone;
    while (true) {
      const FrameId child = leftmost ? nodes_[frame].left : nodes_[frame].right;
      if (child == kNone) return frame;
      frame = child;
    }
  }

  auto Step(FrameId frame, bool forward) const -> FrameId {
    const FrameId child = forward ? nodes_[frame].right : nodes_[frame].left;
    if (child != kNone) return Extreme(child, forward);
    FrameId parent = nodes_[frame].parent;
    while (parent != kNone && frame == (forward ? nodes_[parent].right : nodes_[parent].left)) {
      frame = parent;
      parent = nodes_[parent].parent;
    }
    return parent;
  }

  // Puts `replacement` where `frame` hangs under its parent (or at the root).
  void Replace(FrameId frame, FrameId replacement) {
    const FrameId parent = nodes_[frame].parent;
    if (parent == kNone) *root_ = replacement;
    else if (nodes_[parent].left == frame) nodes_[parent].left = replacement;
    else nodes_[parent].right = replacement;
    if (replacement != kNone) nodes_[replacement].parent = parent;
  }

  // Rotates `frame` above its parent.
  void Rotate(FrameId frame) {
    const FrameId parent = nodes_[frame].parent;
    Replace(parent, frame);
    if (nodes_[parent].left == frame) {
      nodes_[parent].left = nodes_[frame].right;
      if (nodes_[frame].right != kNone) nodes_[nodes_[frame].right].parent = parent;
      nodes_[frame].right = parent;
    } else {
      nodes_[parent].right = nodes_[frame].left;
      if (nodes_[frame].left != kNone) nodes_[nodes_[frame].left].parent = parent;
      nodes_[frame].left = parent;
    }
    nodes_[parent].parent = frame;
  }

  Node *nodes_{nullptr};
  FrameId *root_{nullptr};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_REPLACER_INDEXED_ORDERED_SET_H_
