#include "telepath/facade/page_cache.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

namespace {

constexpr std::size_t kInitialReadaheadBlocks = 4;
constexpr std::uint64_t kNoOffset = UINT64_MAX;

}  // namespace

struct PageCache::FileState {
  std::mutex latch;
  std::atomic<bool> loaded{false};
  std::atomic<bool> touched{false};
  std::atomic<std::uint64_t> size{0};
  std::atomic<std::uint64_t> next_offset{kNoOffset};
  std::atomic<std::size_t> window{0};
  std::atomic<BlockId> prefetched_until{0};
};

auto PageCache::Create(
  PageCacheOptions options,
  std::unique_ptr<DiskBackend> backend,
  std::unique_ptr<Replacer> replacer,
  std::shared_ptr<TelemetrySink> telemetry_sink) -> Result<std::unique_ptr<PageCache>> {
  if (backend == nullptr) return InvalidArgument("page cache needs a disk backend");
  if (options.write_behind) options.buffer.enable_background_cleaner = true;
  if (replacer == nullptr) replacer = MakeClockSweepReplacer(options.buffer.pool_size);
  DiskBackend *raw_backend = backend.get();
  auto manager = BufferManager::Create(options.buffer, std::move(backend), std::move(replacer), std::move(telemetry_sink));
  if (!manager.has_value()) return PropagateError(manager);
  return std::unique_ptr<PageCache>(new PageCache(std::move(options), raw_backend, std::move(manager.value())));
}

PageCache::PageCache(PageCacheOptions options, DiskBackend *backend, std::unique_ptr<BufferManager> manager)
  : options_(std::move(options)), block_size_(manager->page_size()), backend_(backend), manager_(std::move(manager)) {}

PageCache::~PageCache() = default;

auto PageCache::Read(FileId file_id, std::uint64_t offset, std::span<std::byte> out) -> Result<std::size_t> {
  if (closed_.load(std::memory_order_acquire)) return Unavailable("page cache is closed");
  Result<FileState *> state = State(file_id);
  if (!state.has_value()) return PropagateError(state);

  const std::uint64_t size = state.value()->size.load(std::memory_order_acquire);
  if (offset >= size || out.empty()) return std::size_t{0};
  const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size - offset));
  Readahead(file_id, *state.value(), offset, length, size);

  for (std::size_t copied = 0; copied < length;) {
    const std::uint64_t position = offset + copied;
    const std::size_t in_block = static_cast<std::size_t>(position % block_size_);
    const std::size_t chunk = std::min(block_size_ - in_block, length - copied);
    Result<SharedBufferHandle> handle = manager_->ReadBuffer(file_id, position / block_size_);
    if (!handle.has_value()) return PropagateError(handle);
    std::memcpy(out.data() + copied, handle->data().data() + in_block, chunk);
    copied += chunk;
  }
  return length;
}

auto PageCache::Write(FileId file_id, std::uint64_t offset, std::span<const std::byte> data) -> Status {
  if (closed_.load(std::memory_order_acquire)) return Unavailable("page cache is closed");
  if (data.empty()) return Ok();
  Result<FileState *> state = State(file_id);
  if (!state.has_value()) return PropagateError(state);
  FileState &file = *state.value();

  for (std::size_t copied = 0; copied < data.size();) {
    const std::uint64_t position = offset + copied;
    const BlockId block = position / block_size_;
    const std::size_t in_block = static_cast<std::size_t>(position % block_size_);
    const std::size_t chunk = std::min(block_size_ - in_block, data.size() - copied);
    // Only a whole-block overwrite may skip the read: a concurrent writer may
    // already have written, evicted, and persisted part of any other block.
    const ReadBufferMode mode = chunk == block_size_ ? ReadBufferMode::kZeroAndLock : ReadBufferMode::kNormal;
    Result<ExclusiveBufferHandle> handle = manager_->ReadBufferExclusive(file_id, block, nullptr, mode);
    if (!handle.has_value()) return PropagateError(handle);
    std::memcpy(handle->mutable_data().data() + in_block, data.data() + copied, chunk);
    copied += chunk;
  }

  const std::uint64_t end = offset + data.size();
  std::uint64_t size = file.size.load(std::memory_order_relaxed);
  while (size < end && !file.size.compare_exchange_weak(size, end, std::memory_order_acq_rel)) {
  }
  file.touched.store(true, std::memory_order_release);
  return Ok();
}

auto PageCache::FileSize(FileId file_id) -> Result<std::uint64_t> {
  if (closed_.load(std::memory_order_acquire)) return Unavailable("page cache is closed");
  Result<FileState *> state = State(file_id);
  if (!state.has_value()) return PropagateError(state);
  return state.value()->size.load(std::memory_order_acquire);
}

auto PageCache::Truncate(FileId file_id, std::uint64_t size) -> Status {
  if (closed_.load(std::memory_order_acquire)) return Unavailable("page cache is closed");
  Result<FileState *> state = State(file_id);
  if (!state.has_value()) return PropagateError(state);
  FileState &file = *state.value();
  std::lock_guard<std::mutex> guard(file.latch);

  if (size < file.size.load(std::memory_order_acquire)) {
    if (Status dropped = manager_->DropFileBuffers(file_id, (size + block_size_ - 1) / block_size_); !dropped.has_value()) return dropped;
    if (const std::size_t in_block = static_cast<std::size_t>(size % block_size_); in_block != 0) {
      Result<ExclusiveBufferHandle> tail = manager_->ReadBufferExclusive(file_id, size / block_size_);
      if (!tail.has_value()) return PropagateError(tail);
      std::memset(tail->mutable_data().data() + in_block, 0, block_size_ - in_block);
    }
    // Shrink the backing file now, so a later extension cannot expose the old bytes from disk.
    if (Status resized = backend_->SetFileSize(file_id, size); !resized.has_value()) return resized;
  }
  file.size.store(size, std::memory_order_release);
  file.touched.store(true, std::memory_order_release);
  file.next_offset.store(kNoOffset, std::memory_order_relaxed);
  file.prefetched_until.store(0, std::memory_order_relaxed);
  return Ok();
}

auto PageCache::Sync(FileId file_id) -> Status {
  if (closed_.load(std::memory_order_acquire)) return Unavailable("page cache is closed");
  Result<FileState *> state = State(file_id);
  if (!state.has_value()) return PropagateError(state);
  return SyncState(file_id, *state.value());
}

auto PageCache::Close() -> Status {
  if (closed_.exchange(true, std::memory_order_acq_rel)) return Ok();
  std::vector<std::pair<FileId, FileState *>> touched;
  {
    std::shared_lock<std::shared_mutex> guard(files_latch_);
    for (const auto &[file_id, state] : files_) {
      if (state->touched.load(std::memory_order_acquire)) touched.emplace_back(file_id, state.get());
    }
  }
  Status status = Ok();
  for (const auto &[file_id, state] : touched) {
    Status synced = SyncState(file_id, *state);
    if (!synced.has_value() && status.has_value()) status = synced;
  }
  Status closed = manager_->Close();
  if (!closed.has_value() && status.has_value()) status = closed;
  return status;
}

auto PageCache::State(FileId file_id) -> Result<FileState *> {
  FileState *state = nullptr;
  {
    std::shared_lock<std::shared_mutex> guard(files_latch_);
    if (auto it = files_.find(file_id); it != files_.end()) state = it->second.get();
  }
  if (state == nullptr) {
    std::unique_lock<std::shared_mutex> guard(files_latch_);
    auto &slot = files_[file_id];
    if (slot == nullptr) slot = std::make_unique<FileState>();
    state = slot.get();
  }
  if (state->loaded.load(std::memory_order_acquire)) return state;

  std::lock_guard<std::mutex> guard(state->latch);
  if (!state->loaded.load(std::memory_order_relaxed)) {
    Result<std::uint64_t> size = backend_->GetFileSize(file_id);
    if (!size.has_value()) return PropagateError(size);
    state->size.store(size.value(), std::memory_order_relaxed);
    state->loaded.store(true, std::memory_order_release);
  }
  return state;
}

void PageCache::Readahead(FileId file_id, FileState &state, std::uint64_t offset, std::size_t length, std::uint64_t file_size) {
  if (options_.max_readahead_blocks == 0) return;
  const std::uint64_t expected = state.next_offset.exchange(offset + length, std::memory_order_relaxed);
  if (offset != expected) {
    state.window.store(0, std::memory_order_relaxed);
    return;
  }

  std::size_t window = state.window.load(std::memory_order_relaxed);
  window = window == 0 ? std::min(kInitialReadaheadBlocks, options_.max_readahead_blocks) : std::min(window * 2, options_.max_readahead_blocks);
  state.window.store(window, std::memory_order_relaxed);

  const BlockId last_block = (offset + length - 1) / block_size_;
  const BlockId file_blocks = (file_size + block_size_ - 1) / block_size_;
  const BlockId first = std::max(last_block + 1, state.prefetched_until.load(std::memory_order_relaxed));
  const BlockId end = std::min<BlockId>(last_block + 1 + window, file_blocks);
  if (first >= end) return;
  state.prefetched_until.store(end, std::memory_order_relaxed);
  std::ignore = manager_->PrefetchBuffer(file_id, first, end - first);
}

auto PageCache::SyncState(FileId file_id, FileState &state) -> Status {
  std::lock_guard<std::mutex> guard(state.latch);
  if (Status flushed = manager_->FlushFileBuffers(file_id); !flushed.has_value()) return flushed;
  // Writebacks round the backing file up to whole blocks; restore the logical
  // size before making it durable.
  if (Status resized = backend_->SetFileSize(file_id, state.size.load(std::memory_order_acquire)); !resized.has_value()) return resized;
  return manager_->SyncFile(file_id);
}

}  // namespace telepath
