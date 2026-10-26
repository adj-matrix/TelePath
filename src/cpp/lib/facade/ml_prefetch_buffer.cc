#include "telepath/facade/ml_prefetch_buffer.h"

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/replacer/plan_replacer.h"

namespace telepath {

namespace {

constexpr std::size_t kDefaultBlockSize = std::size_t{1} << 20;

}  // namespace

auto MLPrefetchBuffer::Create(
  MLPrefetchBufferOptions options,
  std::unique_ptr<DiskBackend> backend,
  std::shared_ptr<TelemetrySink> telemetry_sink) -> Result<std::unique_ptr<MLPrefetchBuffer>> {
  if (backend == nullptr) return InvalidArgument("ml prefetch buffer needs a disk backend");
  if (options.buffer.page_size == 0) options.buffer.page_size = kDefaultBlockSize;
  options.buffer.enable_background_cleaner = false;
  auto replacer = std::make_unique<PlanReplacer>(options.buffer.pool_size);
  PlanReplacer *raw_replacer = replacer.get();
  auto manager = BufferManager::Create(options.buffer, std::move(backend), std::move(replacer), std::move(telemetry_sink));
  if (!manager.has_value()) return PropagateError(manager);
  return std::unique_ptr<MLPrefetchBuffer>(new MLPrefetchBuffer(std::move(options), raw_replacer, std::move(manager.value())));
}

MLPrefetchBuffer::MLPrefetchBuffer(MLPrefetchBufferOptions options, PlanReplacer *replacer, std::unique_ptr<BufferManager> manager)
  : options_(std::move(options)), block_size_(manager->page_size()), replacer_(replacer), manager_(std::move(manager)) {}

MLPrefetchBuffer::~MLPrefetchBuffer() = default;

auto MLPrefetchBuffer::SetPlan(std::span<const BufferTag> plan) -> Status {
  if (closed_.load(std::memory_order_acquire)) return Unavailable("ml prefetch buffer is closed");
  plan_.assign(plan.begin(), plan.end());
  next_use_.assign(plan.size(), PlanReplacer::kNeverUsed);
  std::unordered_map<BufferTag, std::uint64_t, BufferTagHash> later;
  for (std::size_t position = plan.size(); position-- > 0;) {
    auto [it, inserted] = later.try_emplace(plan[position], position);
    if (!inserted) {
      next_use_[position] = it->second;
      it->second = position;
    }
  }
  cursor_.store(0, std::memory_order_relaxed);
  prefetched_until_.store(0, std::memory_order_relaxed);
  replacer_->ForgetPlan();
  return Ok();
}

auto MLPrefetchBuffer::Fetch(std::size_t position) -> Result<SharedBufferHandle> {
  if (closed_.load(std::memory_order_acquire)) return Unavailable("ml prefetch buffer is closed");
  if (position >= plan_.size()) return InvalidArgument("plan position is out of range");
  Prefetch(position);

  const BufferTag &tag = plan_[position];
  Result<SharedBufferHandle> handle = manager_->ReadBuffer(tag.file_id, tag.block_id);
  if (!handle.has_value()) return PropagateError(handle);
  replacer_->SetNextUse(handle->frame_id(), next_use_[position]);
  return handle;
}

auto MLPrefetchBuffer::Next() -> Result<std::optional<PlannedBlock>> {
  if (closed_.load(std::memory_order_acquire)) return Unavailable("ml prefetch buffer is closed");
  const std::size_t position = cursor_.fetch_add(1, std::memory_order_relaxed);
  if (position >= plan_.size()) return std::optional<PlannedBlock>{};
  Result<SharedBufferHandle> handle = Fetch(position);
  if (!handle.has_value()) return PropagateError(handle);
  return std::optional<PlannedBlock>{PlannedBlock{position, std::move(handle.value())}};
}

auto MLPrefetchBuffer::Close() -> Status {
  if (closed_.load(std::memory_order_acquire)) return Ok();
  Status status = manager_->Close();
  if (status.has_value() || status.error().code() != ErrorCode::kInvalidArgument) closed_.store(true, std::memory_order_release);
  return status;
}

void MLPrefetchBuffer::Prefetch(std::size_t position) {
  if (options_.prefetch_depth == 0) return;
  const std::size_t end = std::min(plan_.size(), position + 1 + options_.prefetch_depth);
  std::size_t observed = prefetched_until_.load(std::memory_order_relaxed);
  std::size_t first = 0;
  do {
    first = std::max(observed, position + 1);
    if (first >= end) return;
  } while (!prefetched_until_.compare_exchange_weak(observed, end, std::memory_order_relaxed));
  for (std::size_t ahead = first; ahead < end; ++ahead) {
    const BufferTag &tag = plan_[ahead];
    std::ignore = manager_->PrefetchBuffer(tag.file_id, tag.block_id, 1);
  }
}

}  // namespace telepath
