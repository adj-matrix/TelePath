#ifndef TELEPATH_FACADE_ML_PREFETCH_BUFFER_H_
#define TELEPATH_FACADE_ML_PREFETCH_BUFFER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "telepath/buffer/buffer_handle.h"
#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "telepath/io/disk_backend.h"
#include "telepath/options/buffer_manager_options.h"

namespace telepath {

class BufferManager;
class PlanReplacer;
class TelemetrySink;

struct MLPrefetchBufferOptions {
  // Kernel configuration. A zero page_size selects 1 MiB blocks; background
  // writeback is always off because the cache is read-only.
  BufferManagerOptions buffer{};
  // Number of plan positions read ahead of the furthest position fetched so
  // far; 0 disables prefetching.
  std::size_t prefetch_depth{8};
};

// One block handed out by MLPrefetchBuffer::Next().
struct PlannedBlock {
  std::size_t position{0};
  SharedBufferHandle handle;
};

// A read-only block cache for datasets read in an order known in advance, such
// as one epoch of ML training, built on one BufferManager that it owns. The
// plan lists the blocks in consumption order; a block may appear more than
// once. The cache prefetches upcoming positions and evicts the block needed
// furthest in the future (Belady), using PlanReplacer.
//
// Blocks are consumed either by position with Fetch(), where the caller (for
// example a data loader distributing positions to its workers
// deterministically) decides which thread takes which position, or with
// Next(), which hands out positions in order to whichever thread asks. Use one
// style per plan: Next() does not know which positions Fetch() consumed.
// Fetch(), Next(), and Close() are thread-safe; SetPlan() must not race with
// them.
class MLPrefetchBuffer {
 public:
  // Creates the cache and its kernel. A null `telemetry_sink` selects the
  // no-op sink.
  static auto Create(
    MLPrefetchBufferOptions options,
    std::unique_ptr<DiskBackend> backend,
    std::shared_ptr<TelemetrySink> telemetry_sink = nullptr) -> Result<std::unique_ptr<MLPrefetchBuffer>>;

  MLPrefetchBuffer(const MLPrefetchBuffer &) = delete;
  MLPrefetchBuffer &operator=(const MLPrefetchBuffer &) = delete;
  ~MLPrefetchBuffer();

  // Starts a new plan, for example the shuffled block order of the next
  // epoch, and resets the Next() cursor. Pages cached for the previous plan
  // stay resident until evicted.
  auto SetPlan(std::span<const BufferTag> plan) -> Status;
  // Returns the block at `position` of the current plan, pinned for reading,
  // and prefetches the positions after it. Returns kInvalidArgument for a
  // position outside the plan.
  auto Fetch(std::size_t position) -> Result<SharedBufferHandle>;
  // Claims the next position not yet handed out by Next() and fetches it;
  // returns nullopt once every position has been handed out.
  auto Next() -> Result<std::optional<PlannedBlock>>;
  // Closes the cache; afterwards every call returns ErrorCode::kUnavailable.
  // Fails with kInvalidArgument while handles are still alive.
  auto Close() -> Status;

  auto plan_size() const -> std::size_t { return plan_.size(); }
  auto block_size() const -> std::size_t { return block_size_; }

 private:
  MLPrefetchBuffer(MLPrefetchBufferOptions options, PlanReplacer *replacer, std::unique_ptr<BufferManager> manager);

  void Prefetch(std::size_t position);

  MLPrefetchBufferOptions options_;
  std::size_t block_size_;
  PlanReplacer *replacer_;
  std::unique_ptr<BufferManager> manager_;
  std::atomic<bool> closed_{false};
  std::vector<BufferTag> plan_;
  // For each position, the next position of the same block, or
  // PlanReplacer::kNeverUsed.
  std::vector<std::uint64_t> next_use_;
  std::atomic<std::size_t> cursor_{0};
  std::atomic<std::size_t> prefetched_until_{0};
};

}  // namespace telepath

#endif  // TELEPATH_FACADE_ML_PREFETCH_BUFFER_H_
