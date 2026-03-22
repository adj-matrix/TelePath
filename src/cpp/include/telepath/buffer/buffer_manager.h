#ifndef TELEPATH_BUFFER_BUFFER_MANAGER_H_
#define TELEPATH_BUFFER_BUFFER_MANAGER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <condition_variable>

#include "telepath/buffer/buffer_descriptor.h"
#include "telepath/buffer/buffer_handle.h"
#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "telepath/io/disk_backend.h"
#include "telepath/options/buffer_manager_options.h"

namespace telepath {

class DiskBackend;
class FrameMemoryPool;
class Replacer;
class TelemetrySink;

struct FrameReservation {
  FrameId frame_id{kInvalidFrameId};
  bool had_evicted_page{false};
  BufferTag evicted_tag{};
  bool evicted_dirty{false};
  uint64_t evicted_dirty_generation{0};
};

class BufferManager {
 public:
  BufferManager(const BufferManagerOptions &options,
                std::unique_ptr<DiskBackend> disk_backend,
                std::unique_ptr<Replacer> replacer,
                std::shared_ptr<TelemetrySink> telemetry_sink);
  BufferManager(std::size_t pool_size, std::size_t page_size,
                std::unique_ptr<DiskBackend> disk_backend,
                std::unique_ptr<Replacer> replacer,
                std::shared_ptr<TelemetrySink> telemetry_sink);
  ~BufferManager();

  Result<BufferHandle> ReadBuffer(FileId file_id, BlockId block_id);
  Status ReleaseBuffer(BufferHandle &&handle);
  Status MarkBufferDirty(const BufferHandle &handle);
  Status FlushBuffer(const BufferHandle &handle);
  Status FlushAll();

  std::size_t pool_size() const { return pool_size_; }
  std::size_t page_size() const { return page_size_; }
  const BufferManagerOptions &options() const { return options_; }

 private:
  friend class BufferHandle;

  Status ReleaseFrame(FrameId frame_id);
  Status FlushFrame(FrameId frame_id);
  Status FlushFrameWithStableSource(FrameId frame_id,
                                    const std::byte *stable_data);
  Result<FrameReservation> ReserveFrameForTag(const BufferTag &tag);
  Result<FrameId> CompleteReservation(const BufferTag &tag,
                                      const FrameReservation &reservation);
  Status RestoreFrameAfterFailedEviction(FrameId frame_id, const BufferTag &old_tag,
                                        bool was_dirty,
                                        uint64_t dirty_generation);
  bool ValidateFrame(FrameId frame_id) const;
  bool ValidateHandle(const BufferHandle &handle) const;
  Result<BufferHandle> AwaitResidentBuffer(FrameId frame_id, const BufferTag &tag);
  std::optional<BufferHandle> TryReadResidentBuffer(const BufferTag &tag);
  std::size_t GetPageTableStripe(const BufferTag &tag) const;
  Status WaitForDiskRequest(uint64_t request_id);
  const std::byte *AcquireReadPointer(BufferHandle *handle) const;
  std::byte *AcquireWritePointer(BufferHandle *handle);
  std::byte *GetFrameData(FrameId frame_id);
  const std::byte *GetFrameData(FrameId frame_id) const;

  std::size_t pool_size_{0};
  std::size_t page_size_{0};
  BufferManagerOptions options_{};
  std::unique_ptr<DiskBackend> disk_backend_;
  std::unique_ptr<Replacer> replacer_;
  std::shared_ptr<TelemetrySink> telemetry_sink_;
  Status init_status_{Status::Ok()};

  std::unique_ptr<FrameMemoryPool> frame_pool_;
  std::vector<BufferDescriptor> descriptors_;

  std::mutex miss_latch_;
  std::mutex free_list_latch_;
  std::mutex poll_latch_;
  mutable std::mutex completion_latch_;
  std::condition_variable completion_cv_;
  std::unordered_map<uint64_t, DiskCompletion> completion_cache_;
  std::vector<std::mutex> page_table_latches_;
  std::unordered_map<BufferTag, FrameId, BufferTagHash> page_table_;
  std::vector<FrameId> free_list_;
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_MANAGER_H_
