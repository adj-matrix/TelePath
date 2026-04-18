#ifndef TELEPATH_BUFFER_BUFFER_MANAGER_H_
#define TELEPATH_BUFFER_BUFFER_MANAGER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

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
class BufferManagerObserver;
class BufferManagerCompletionDispatcher;
class BufferManagerMissCoordinator;
class BufferManagerFlushScheduler;
class BufferManagerCleanerController;
class BufferManagerPageTable;
struct BufferManagerMissState;
struct BufferManagerFlushTask;

struct FrameReservation {
  FrameId frame_id{kInvalidFrameId};
  bool had_evicted_page{false};
  BufferTag evicted_tag{};
  bool evicted_dirty{false};
  uint64_t evicted_dirty_generation{0};
};

struct FrameSnapshot {
  FrameId frame_id{kInvalidFrameId};
  BufferFrameState state{BufferFrameState::kFree};
  BufferTag tag{};
  uint32_t pin_count{0};
  uint64_t dirty_generation{0};
  bool is_valid{false};
  bool is_dirty{false};
  bool io_in_flight{false};
  bool flush_queued{false};
  bool flush_in_flight{false};
};

struct BufferPoolSnapshot {
  std::size_t pool_size{0};
  std::size_t page_size{0};
  std::vector<FrameSnapshot> frames;
};

class BufferManager {
 public:
  // Builds a buffer manager from explicit options. Initialization failures are
  // surfaced through subsequent API calls as Status values.
  BufferManager(
    const BufferManagerOptions &options,
    std::unique_ptr<DiskBackend> disk_backend,
    std::unique_ptr<Replacer> replacer,
    std::shared_ptr<TelemetrySink> telemetry_sink);
  // Convenience overload for callers that only want pool/page sizing.
  BufferManager(
    std::size_t pool_size, std::size_t page_size,
    std::unique_ptr<DiskBackend> disk_backend,
    std::unique_ptr<Replacer> replacer,
    std::shared_ptr<TelemetrySink> telemetry_sink);
  ~BufferManager();

  // Pins the requested page in memory and returns a scoped handle to it. The
  // page may be loaded from disk or served from the resident cache.
  Result<BufferHandle> ReadBuffer(FileId file_id, BlockId block_id);
  // Explicitly releases a handle before it goes out of scope.
  Status ReleaseBuffer(BufferHandle &&handle);
  // Marks the page referenced by the handle as dirty so that it will be
  // written back before eviction or during an explicit flush.
  Status MarkBufferDirty(const BufferHandle &handle);
  // Flushes the page referenced by the handle to the backing store.
  Status FlushBuffer(const BufferHandle &handle);
  // Flushes every currently resident dirty page.
  Status FlushAll();
  // Returns a point-in-time view of frame metadata for observability and debug
  // tooling. This does not expose direct page memory access.
  BufferPoolSnapshot ExportSnapshot() const;

  std::size_t pool_size() const { return pool_size_; }
  std::size_t page_size() const { return page_size_; }
  // Returns the resolved runtime options, including derived defaults.
  const BufferManagerOptions &options() const { return options_; }

 private:
  friend class BufferHandle;

  Status ReleaseFrame(FrameId frame_id);
  auto BuildPinnedHandle(FrameId frame_id, const BufferTag &tag) -> BufferHandle;
  auto UnpinFrame(FrameId frame_id) -> Result<bool>;
  bool ShouldQueueCleanerCandidate(const BufferDescriptor &descriptor) const;
  auto TryReadResidentHit(const BufferTag &tag) -> std::optional<BufferHandle>;
  auto AwaitJoinedMiss(
    const BufferTag &tag,
    const std::shared_ptr<BufferManagerMissState> &state
  ) -> Result<BufferHandle>;
  auto TryServeMissOwnerHit(
    const BufferTag &tag,
    const std::shared_ptr<BufferManagerMissState> &state
  ) -> std::optional<BufferHandle>;
  auto LoadMissOwnerBuffer(
    const BufferTag &tag,
    const std::shared_ptr<BufferManagerMissState> &state
  ) -> Result<BufferHandle>;
  void CompleteMiss(
    const BufferTag &tag,
    const std::shared_ptr<BufferManagerMissState> &state,
    const Status &status, FrameId frame_id);
  auto ValidateOwnedHandle(const BufferHandle &handle) const -> Status;
  auto MarkFrameDirty(FrameId frame_id, const BufferTag &tag) -> Result<bool>;
  bool HandleHasStableFlushSource(const BufferHandle &handle) const;
  void RecordPinnedAccess(FrameId frame_id);
  auto TryPinResidentFrame(FrameId frame_id, const BufferTag &tag)
    -> std::optional<BufferHandle>;
  auto WaitForFrameReady(FrameId frame_id) -> Status;
  auto WaitForPendingFlush(
    BufferDescriptor *descriptor,
    std::unique_lock<std::mutex> *descriptor_guard,
    const std::byte *stable_data, bool *was_busy
  ) -> Result<bool>;
  void FinishFlushCompletion(
    BufferDescriptor *descriptor,
    const BufferManagerFlushTask &task,
    const Status &flush_status,
    bool *cleared_dirty,
    bool *should_requeue_cleaner);
  auto PrepareFlushTask(
    FrameId frame_id,
    const std::byte *stable_data,
    bool *was_busy,
    bool cleaner_owned,
    BufferTag *tag,
    uint64_t *dirty_generation
  ) -> Result<bool>;
  auto CaptureFlushSnapshot(FrameId frame_id, const std::byte *stable_data)
    -> std::vector<std::byte>;
  auto BuildDirtyFlushTask(
    FrameId frame_id,
    const BufferTag &tag,
    uint64_t dirty_generation,
    const std::byte *stable_data,
    bool cleaner_owned
  ) -> std::shared_ptr<BufferManagerFlushTask>;
  auto BuildEvictedFlushTask(const FrameReservation &reservation)
    -> std::shared_ptr<BufferManagerFlushTask>;
  auto EnqueueFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task)
    -> Status;
  auto RollBackFlushTask(
    FrameId frame_id,
    bool cleaner_owned,
    const Status &status
  ) -> Status;
  auto WaitForFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task)
    -> Status;
  auto FlushReservedVictim(const FrameReservation &reservation) -> Status;
  auto SubmitReadRequest(FrameId frame_id, const BufferTag &tag)
    -> Result<uint64_t>;
  auto WaitForReadRequest(uint64_t request_id) -> Status;
  void FinalizeCleanerFlush(const std::shared_ptr<BufferManagerFlushTask> &task);
  void AcquireReadLatch(BufferHandle *handle) const;
  void AcquireWriteLatch(BufferHandle *handle);
  auto WaitForScheduledFlushes(
    const std::vector<std::shared_ptr<BufferManagerFlushTask>> &tasks
  ) -> Status;
  auto FlushBusyFrames(const std::vector<FrameId> &frame_ids) -> Status;
  void BeginFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task);
  void ResetCleanerCandidate(FrameId frame_id);
  void MaybeEnqueueCleanerCandidate(FrameId frame_id);
  bool ShouldSeedCleanerCandidate(FrameId frame_id) const;
  void SeedCleanerCandidates();
  Status FlushFrame(FrameId frame_id);
  auto TryBuildFlushTask(
    FrameId frame_id,
    const std::byte *stable_data,
    bool *was_busy,
    bool cleaner_owned
  ) -> Result<std::shared_ptr<BufferManagerFlushTask>>;
  auto RunForegroundFlush(FrameId frame_id, const std::byte *stable_data)
    -> Status;
  Status FlushFrameWithStableSource(
    FrameId frame_id,
    const std::byte *stable_data);
  Result<std::shared_ptr<BufferManagerFlushTask>> TryScheduleFlushTask(
    FrameId frame_id,
    const std::byte *stable_data,
    bool *was_busy,
    bool cleaner_owned);
  Status FlushEvictedPage(const FrameReservation &reservation);
  void FinalizeFlushTask(
    const std::shared_ptr<BufferManagerFlushTask> &task,
    const Status &flush_status);
  auto InitializeFramePool() -> Status;
  void SeedFreeFrames();
  auto ValidateRuntimeDependencies() const -> Status;
  void ResolveCleanerOptions();
  void ResolveFlushOptions();
  auto ResolveFlushWorkerCount(
    const DiskBackendCapabilities &capabilities
  ) const -> std::size_t;
  auto ResolveFlushSubmitBatchSize(
    const DiskBackendCapabilities &capabilities,
    std::size_t flush_worker_count
  ) const -> std::size_t;
  auto ResolveFlushForegroundBurstLimit() const -> std::size_t;
  void ScheduleCleanerFlush(FrameId frame_id);
  auto StartFlushScheduler() -> Status;
  auto StartCleanerController() -> Status;
  void StopFlushPipeline(const Status &reason);
  auto AcquireReservationFrame() -> Result<FrameId>;
  void RequeueReservationCandidate(FrameId frame_id);
  auto LookupFrameId(const BufferTag &tag) -> std::optional<FrameId>;
  void InstallReservationEntry(
    const BufferTag &tag,
    const FrameReservation &reservation);
  auto TryReserveFrameForTag(FrameId frame_id, const BufferTag &tag)
    -> std::optional<FrameReservation>;
  void RemoveReservationEntry(FrameId frame_id, const BufferTag &tag);
  void PrepareLoadingFrame(
    const BufferTag &tag,
    const FrameReservation &reservation,
    BufferDescriptor *descriptor);
  void MarkFrameReadInFlight(FrameId frame_id);
  void CompleteLoadedFrame(FrameId frame_id);
  void ResetFrameToFree(
    FrameId frame_id,
    BufferDescriptor *descriptor,
    const Status &status);
  void ReleaseFreeFrame(FrameId frame_id);
  auto AbortLoadReservation(
    FrameId frame_id,
    const BufferTag &tag,
    const Status &status
  ) -> Status;
  auto ReadReservedPage(FrameId frame_id, const BufferTag &tag) -> Status;
  auto RestoreEvictionFailure(
    const FrameReservation &reservation,
    const Status &status
  ) -> Status;
  void RestoreEvictedMapping(FrameId frame_id, const BufferTag &old_tag);
  void RestoreDirtyEviction(FrameId frame_id);
  void FinishEvictionFailureRecovery(FrameId frame_id);
  Result<FrameReservation> ReserveFrameForTag(const BufferTag &tag);
  Result<FrameId> CompleteReservation(
    const BufferTag &tag,
    const FrameReservation &reservation);
  Status RestoreFrameAfterFailedEviction(
    FrameId frame_id,
    const BufferTag &old_tag,
    bool was_dirty,
    uint64_t dirty_generation);
  Status StartCompletionDispatcher();
  bool ValidateFrame(FrameId frame_id) const;
  bool ValidateHandle(const BufferHandle &handle) const;
  Result<BufferHandle> AwaitResidentBuffer(FrameId frame_id, const BufferTag &tag);
  std::optional<BufferHandle> TryReadResidentBuffer(const BufferTag &tag);
  void NotifyCleaner();
  const std::byte *AcquireReadPointer(BufferHandle *handle) const;
  std::byte *AcquireWritePointer(BufferHandle *handle);
  std::byte *GetFrameData(FrameId frame_id);
  const std::byte *GetFrameData(FrameId frame_id) const;

  std::size_t pool_size_{0};
  std::size_t page_size_{0};
  BufferManagerOptions options_{};
  std::unique_ptr<DiskBackend> disk_backend_;
  std::unique_ptr<Replacer> replacer_;
  std::unique_ptr<BufferManagerObserver> observer_;
  Status init_status_{Status::Ok()};

  std::unique_ptr<FrameMemoryPool> frame_pool_;
  std::unique_ptr<BufferManagerMissCoordinator> miss_coordinator_;
  std::unique_ptr<BufferManagerCompletionDispatcher> completion_dispatcher_;
  std::unique_ptr<BufferManagerFlushScheduler> flush_scheduler_;
  std::unique_ptr<BufferManagerCleanerController> cleaner_controller_;
  std::unique_ptr<BufferManagerPageTable> page_table_;
  std::vector<BufferDescriptor> descriptors_;

  std::mutex free_list_latch_;
  std::atomic<std::size_t> dirty_page_count_{0};
  std::vector<FrameId> free_list_;
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_MANAGER_H_
