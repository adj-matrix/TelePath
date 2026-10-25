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
#include "telepath/common/block_geometry.h"
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
class BufferManagerTestPeer;
struct BufferManagerMissState;
struct BufferManagerFlushTask;

// A reserved frame never carries a dirty victim: dirty victims are written back
// while still mapped under their old tag, before the frame can be reserved.
struct FrameReservation {
  FrameId frame_id{kInvalidFrameId};
  bool had_evicted_page{false};
  BufferTag evicted_tag{};
};

enum class FrameReservationOutcome {
  kReserved = 0,
  kNeedsFlush,
  kRejected,
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
  std::size_t dirty_page_count{0};
  std::size_t flush_queued_count{0};
  std::size_t flush_in_flight_count{0};
  std::vector<FrameSnapshot> frames;
};

class BufferManager {
 public:
  // Creates a fully initialized buffer manager, or returns the error that
  // prevented initialization: invalid options (including a page size that is
  // not a valid BlockGeometry size), a backend whose block size differs from
  // `options.page_size`, missing dependencies, or a background component that
  // failed to start. A null telemetry sink selects
  // the no-op sink.
  static auto Create(
    const BufferManagerOptions &options,
    std::unique_ptr<DiskBackend> disk_backend,
    std::unique_ptr<Replacer> replacer,
    std::shared_ptr<TelemetrySink> telemetry_sink) -> Result<std::unique_ptr<BufferManager>>;
  // Convenience overload for callers that only want pool/page sizing.
  static auto Create(
    std::size_t pool_size, std::size_t page_size,
    std::unique_ptr<DiskBackend> disk_backend,
    std::unique_ptr<Replacer> replacer,
    std::shared_ptr<TelemetrySink> telemetry_sink) -> Result<std::unique_ptr<BufferManager>>;

  BufferManager(const BufferManager &) = delete;
  BufferManager &operator=(const BufferManager &) = delete;
  ~BufferManager();

  // Returns the fatal error that moved this instance into the Failed state, or
  // nullopt while it is healthy. Once failed, every operation returns
  // ErrorCode::kFailed.
  auto failure() const -> std::optional<Error>;

  // Pins the requested page with shared access, loading it from the backing
  // store on a miss. Blocks while another thread holds the page exclusively.
  auto ReadBuffer(FileId file_id, BlockId block_id) -> Result<SharedBufferHandle>;
  // Pins the requested page with exclusive access, loading it from the backing
  // store on a miss. Blocks while any other handle on the page exists.
  auto ReadBufferExclusive(FileId file_id, BlockId block_id) -> Result<ExclusiveBufferHandle>;
  // Explicitly releases a handle before it goes out of scope. Returns
  // kInvalidArgument for an empty handle or one owned by another manager.
  auto ReleaseBuffer(SharedBufferHandle &&handle) -> Status;
  auto ReleaseBuffer(ExclusiveBufferHandle &&handle) -> Status;
  // Writes the page back and waits until it is clean on the backing store.
  auto FlushBuffer(const SharedBufferHandle &handle) -> Status;
  // Writes the page's current bytes back once and waits for completion. The
  // page stays dirty while the handle is alive, because the holder may still
  // modify it; a later flush after release cleans it.
  auto FlushBuffer(ExclusiveBufferHandle &handle) -> Status;
  // Writes the page back if it is resident and dirty, and waits until it is
  // clean. Needs no handle: the page is pinned, not latched, so writers may
  // keep modifying it and their changes are included in later rounds. Returns
  // Ok when the page is not resident. Must not be called by a thread that holds
  // an ExclusiveBufferHandle on the page.
  auto FlushBuffer(FileId file_id, BlockId block_id) -> Status;
  // Flushes every currently resident dirty page. Must not be called by a thread
  // that holds an ExclusiveBufferHandle.
  auto FlushAll() -> Status;
  // Returns a point-in-time view of frame metadata for observability and debug
  // tooling. This does not expose direct page memory access.
  auto ExportSnapshot() const -> BufferPoolSnapshot;

  std::size_t pool_size() const { return pool_size_; }
  std::size_t page_size() const { return geometry_.block_size(); }
  // Validated block geometry shared by frames and the disk backend.
  auto geometry() const -> BlockGeometry { return geometry_; }
  // Returns the resolved runtime options, including derived defaults.
  const BufferManagerOptions &options() const { return options_; }

 private:
  friend class SharedBufferHandle;
  friend class ExclusiveBufferHandle;
  friend class BufferManagerTestPeer;

  BufferManager(
    const BufferManagerOptions &options,
    BlockGeometry geometry,
    std::unique_ptr<DiskBackend> disk_backend,
    std::unique_ptr<Replacer> replacer,
    std::shared_ptr<TelemetrySink> telemetry_sink);

  // buffer_manager_read.cc
  auto PinPage(const BufferTag &tag) -> Result<FrameId>;
  auto TryReadResidentHit(const BufferTag &tag) -> std::optional<FrameId>;
  auto AwaitJoinedMiss(
    const BufferTag &tag,
    const std::shared_ptr<BufferManagerMissState> &state
  ) -> Result<FrameId>;
  auto LoadMissOwnerBuffer(
    const BufferTag &tag,
    const std::shared_ptr<BufferManagerMissState> &state
  ) -> Result<FrameId>;
  bool TryPinResidentFrame(FrameId frame_id, const BufferTag &tag);
  auto ReserveFrameForTag(const BufferTag &tag) -> Result<FrameReservation>;
  auto CompleteReservation(
    const BufferTag &tag,
    const FrameReservation &reservation
  ) -> Result<FrameId>;
  void CompleteLoadedFrame(FrameId frame_id);
  auto ReadReservedPage(FrameId frame_id, const BufferTag &tag) -> Status;
  auto LoadTag(const BufferDescriptor &descriptor) const -> BufferTag;
  void StoreTag(BufferDescriptor *descriptor, const BufferTag &tag);

  // buffer_manager_cleaner.cc
  void ResetCleanerCandidate(FrameId frame_id);
  void NotifyCleaner();
  void ScheduleCleanerFlush(FrameId frame_id);
  auto StartCleanerController() -> Status;
  static bool ShouldQueueCleanerCandidate(std::uint32_t control_word);
  void MaybeEnqueueCleanerCandidate(FrameId frame_id);
  bool ShouldSeedCleanerCandidate(FrameId frame_id) const;
  void SeedCleanerCandidates();

  // buffer_manager_flush.cc
  auto FlushFrameWithStableSource(
    FrameId frame_id,
    const std::byte *stable_data
  ) -> Status;
  auto FlushFrame(FrameId frame_id) -> Status;
  auto RunForegroundFlush(FrameId frame_id, const std::byte *stable_data) -> Status;
  auto FlushFrameOnce(FrameId frame_id, const std::byte *stable_data) -> Status;
  auto WaitForScheduledFlushes(const std::vector<std::shared_ptr<BufferManagerFlushTask>> &tasks) -> Status;
  auto FlushBusyFrames(const std::vector<FrameId> &frame_ids) -> Status;
  auto TryScheduleFlushTask(
    FrameId frame_id,
    const std::byte *stable_data,
    bool *was_busy,
    bool cleaner_owned
  ) -> Result<std::shared_ptr<BufferManagerFlushTask>>;
  auto PrepareFlushTask(
    FrameId frame_id,
    const std::byte *stable_data,
    bool *was_busy,
    bool cleaner_owned,
    BufferTag *tag,
    std::vector<std::byte> *snapshot
  ) -> Result<bool>;
  auto CaptureFlushSnapshot(FrameId frame_id, const std::byte *stable_data) -> std::vector<std::byte>;
  auto EnqueueFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task) -> Status;
  auto RollBackFlushTask(
    FrameId frame_id,
    bool cleaner_owned,
    const Status &status
  ) -> Status;
  auto WaitForPendingFlush(BufferDescriptor *descriptor) -> Status;
  void BeginFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task);
  void FinalizeFlushTask(
    const std::shared_ptr<BufferManagerFlushTask> &task,
    const Status &flush_status);

  // buffer_manager_eviction.cc
  auto AbortLoadReservation(
    FrameId frame_id,
    const BufferTag &tag,
    const Status &status
  ) -> Status;
  void ReleaseFreeFrame(FrameId frame_id);
  auto AcquireReservationFrame() -> Result<FrameId>;
  void InstallReservationEntry(
    const BufferTag &tag,
    const FrameReservation &reservation);
  auto TryReserveFrameForTag(
    FrameId frame_id,
    const BufferTag &tag,
    bool allow_victim_flush,
    FrameReservation *reservation
  ) -> FrameReservationOutcome;
  void ReturnUnreservedVictim(FrameId frame_id);
  auto FlushVictimBeforeReuse(FrameId frame_id) -> Status;

  // buffer_manager_handle.cc
  auto MakeSharedHandle(FrameId frame_id, const BufferTag &tag) -> SharedBufferHandle;
  auto MakeExclusiveHandle(FrameId frame_id, const BufferTag &tag) -> ExclusiveBufferHandle;
  void ReleaseSharedHandle(FrameId frame_id) noexcept;
  void ReleaseExclusiveHandle(FrameId frame_id) noexcept;
  void MarkHandleDirty(FrameId frame_id) noexcept;
  void ReleaseFrame(FrameId frame_id) noexcept;

  // buffer_manager_lifecycle.cc
  auto Initialize() -> Status;
  auto CheckUsable() const -> Status;
  void EnterFailedState(const Error &cause);
  auto InitializeFramePool() -> Status;
  void SeedFreeFrames();
  auto ValidateRuntimeDependencies() const -> Status;
  void ResolveCleanerOptions();
  auto ResolveFlushWorkerCount(const DiskBackendCapabilities &capabilities) const -> std::size_t;
  auto ResolveFlushSubmitBatchSize(
    const DiskBackendCapabilities &capabilities,
    std::size_t flush_worker_count
  ) const -> std::size_t;
  auto ResolveFlushForegroundBurstLimit() const -> std::size_t;
  auto StartFlushScheduler() -> Status;
  void StopFlushPipeline(const Status &reason);
  auto StartCompletionDispatcher() -> Status;

  std::size_t pool_size_{0};
  BlockGeometry geometry_;
  BufferManagerOptions options_{};
  std::unique_ptr<DiskBackend> disk_backend_;
  std::unique_ptr<Replacer> replacer_;
  std::unique_ptr<BufferManagerObserver> observer_;
  std::atomic<bool> failed_{false};
  mutable std::mutex failure_latch_;
  std::optional<Error> failure_;

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
