#include "telepath/buffer/buffer_manager.h"

#include <mutex>
#include <utility>

#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::CreateNamespace(NamespaceOptions options) -> Result<BufferNamespace> {
  if (Status usable = CheckUsable(); !usable.has_value()) return PropagateError(usable);
  if (segment_ != nullptr) return Unavailable("namespaces are not supported in cross-process mode yet");
  if (options.name.empty()) return InvalidArgument("namespace name must not be empty");
  if (options.replacer == nullptr) return InvalidArgument("namespace replacer must not be null");
  if (options.max_frames != 0 && options.min_frames > options.max_frames) return InvalidArgument("namespace minimum exceeds its maximum");

  std::lock_guard<std::mutex> guard(namespace_latch_);
  const std::size_t count = namespace_count_.load(std::memory_order_relaxed);
  std::size_t guaranteed = options.min_frames;
  for (std::size_t index = 0; index < count; ++index) {
    if (namespaces_[index].name == options.name) return InvalidArgument("namespace name is already taken");
    guaranteed += namespaces_[index].min_frames;
  }
  if (guaranteed > pool_size_) return InvalidArgument("namespace guarantees exceed the pool");
  if (count == kMaxNamespaces) return ResourceExhausted("too many namespaces");

  NamespaceState &state = namespaces_[count];
  state.name = std::move(options.name);
  state.replacer = std::move(options.replacer);
  state.min_frames = options.min_frames;
  state.max_frames = options.max_frames;
  state.pre_write_hook = std::move(options.pre_write_hook);
  replacers_[count] = state.replacer.get();
  namespace_count_.store(count + 1, std::memory_order_release);
  return BufferNamespace(this, static_cast<NamespaceId>(count));
}

void BufferManager::AssignFrame(FrameId frame_id, NamespaceId space) {
  std::atomic<NamespaceId> &owner = descriptors_[frame_id].namespace_id;
  const NamespaceId previous = owner.load(std::memory_order_relaxed);
  if (previous == space) return;
  if (previous != kNoNamespace) {
    namespaces_[previous].owned.fetch_sub(1, std::memory_order_relaxed);
    replacers_[previous]->Remove(frame_id);
  }
  owner.store(space, std::memory_order_relaxed);
  namespaces_[space].owned.fetch_add(1, std::memory_order_relaxed);
}

void BufferManager::ForgetFrame(FrameId frame_id) {
  std::atomic<NamespaceId> &owner = descriptors_[frame_id].namespace_id;
  const NamespaceId previous = owner.exchange(kNoNamespace, std::memory_order_relaxed);
  if (previous == kNoNamespace) return;
  replacers_[previous]->Remove(frame_id);
  namespaces_[previous].owned.fetch_sub(1, std::memory_order_relaxed);
}

auto BufferManager::PreWriteHookOf(FrameId frame_id) const -> const PreWriteHook & {
  const NamespaceId owner = descriptors_[frame_id].namespace_id.load(std::memory_order_relaxed);
  if (owner == kDefaultNamespace || owner >= kMaxNamespaces) return options_.pre_write_hook;
  return namespaces_[owner].pre_write_hook;
}

auto BufferNamespace::ReadBuffer(FileId file_id, BlockId block_id, BufferAccessStrategy *strategy) -> Result<SharedBufferHandle> {
  return manager_->ReadBufferIn(id_, file_id, block_id, strategy);
}

auto BufferNamespace::ReadBufferExclusive(FileId file_id, BlockId block_id, BufferAccessStrategy *strategy, ReadBufferMode mode) -> Result<ExclusiveBufferHandle> {
  return manager_->ReadBufferExclusiveIn(id_, file_id, block_id, strategy, mode);
}

auto BufferNamespace::PrefetchBuffer(FileId file_id, BlockId block_id, std::size_t block_count) -> Status {
  return manager_->PrefetchBufferIn(id_, file_id, block_id, block_count);
}

auto BufferNamespace::StartReadBuffers(ReadBuffersOperation &operation, FileId file_id, BlockId block_id, std::size_t block_count, BufferAccessStrategy *strategy) -> Status {
  return manager_->StartReadBuffersIn(id_, operation, file_id, block_id, block_count, strategy);
}

}  // namespace telepath
