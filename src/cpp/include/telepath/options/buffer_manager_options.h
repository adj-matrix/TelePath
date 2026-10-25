#ifndef TELEPATH_OPTIONS_BUFFER_MANAGER_OPTIONS_H_
#define TELEPATH_OPTIONS_BUFFER_MANAGER_OPTIONS_H_

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <thread>

#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "telepath/io/disk_backend_options.h"

namespace telepath {

// Called with a stable copy of a page right before it is written back, for
// example to flush the write-ahead log up to the page's LSN. An error fails
// that writeback and keeps the page dirty. Runs on the thread that schedules
// the writeback and may block; it must not call back into the BufferManager.
using PreWriteHook = std::function<Status(const BufferTag &tag, std::span<const std::byte> page)>;

// Reserved for the observation plane: frame states and counters published in
// shared memory for other processes to read. Not implemented yet;
// BufferManager::Create() rejects `enabled` with ErrorCode::kUnavailable.
struct ObservationOptions {
  bool enabled{false};
  // POSIX shared-memory object name, such as "/telepath_observation".
  std::string shared_memory_name;
};

// How a single-page flush requested by a caller reaches the backend: the
// writeback of a dirty eviction victim and every FlushBuffer() overload.
// FlushAll() and the background cleaner always go through the flush workers.
enum class ForegroundWriteback {
  // The calling thread submits the write and waits for it itself, as
  // PostgreSQL does. No thread hand-off, so the lowest latency.
  kDirect = 0,
  // The write joins the flush workers' foreground queue, sharing their batching
  // and the foreground/background burst limit with background writeback.
  kQueued,
};

// Runtime configuration for BufferManager construction.
struct BufferManagerOptions {
  std::size_t pool_size{0};
  std::size_t page_size{0};
  // Number of striped latches protecting the page table. Set to zero to let
  // the library derive a machine-sensitive default.
  std::size_t page_table_stripe_count{0};
  DiskBackendOptions disk_backend{};
  // Number of background flush workers. Set to zero to let the library derive
  // a backend-sensitive default.
  std::size_t flush_worker_count{0};
  // Maximum number of writeback requests a flush worker may submit before it
  // starts waiting for completions. Set to zero to derive a backend-sensitive
  // default.
  std::size_t flush_submit_batch_size{0};
  // Maximum number of consecutive foreground flush tasks a worker may serve
  // while background writeback is pending. Set to zero to derive a default.
  std::size_t flush_foreground_burst_limit{0};
  // Enables proactive background flushing for evictable dirty pages.
  bool enable_background_cleaner{false};
  // Dirty-page count that wakes the background cleaner. Set to zero to derive
  // a pool-size-sensitive default.
  std::size_t dirty_page_high_watermark{0};
  // Dirty-page count that the background cleaner tries to converge to after a
  // wakeup. Set to zero to derive a value below the high watermark.
  std::size_t dirty_page_low_watermark{0};
  // See ForegroundWriteback.
  ForegroundWriteback foreground_writeback{ForegroundWriteback::kDirect};
  // Optional hook run before every page writeback.
  PreWriteHook pre_write_hook{};
  // Reserved; see ObservationOptions.
  ObservationOptions observation{};

  // Returns the effective stripe count after applying the default policy.
  auto ResolvePageTableStripeCount() const -> std::size_t {
    if (page_table_stripe_count != 0) return page_table_stripe_count;

    const std::size_t hardware_threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const std::size_t baseline = std::max<std::size_t>(16, hardware_threads * 4);
    return std::min<std::size_t>(baseline, std::max<std::size_t>(1, pool_size));
  }

  auto ResolveDirtyPageHighWatermark() const -> std::size_t {
    if (pool_size == 0) return 0;
    if (dirty_page_high_watermark != 0) return std::min<std::size_t>(dirty_page_high_watermark, pool_size);
    return std::max<std::size_t>(1, (pool_size * 3) / 4);
  }

  auto ResolveDirtyPageLowWatermark() const -> std::size_t {
    const std::size_t high = ResolveDirtyPageHighWatermark();
    if (high == 0) return 0;
    const std::size_t max_low = high - 1;
    if (dirty_page_low_watermark != 0) return std::min<std::size_t>(dirty_page_low_watermark, max_low);
    if (high == 1) return 0;
    return std::min<std::size_t>(high / 2, max_low);
  }
};

}  // namespace telepath

#endif  // TELEPATH_OPTIONS_BUFFER_MANAGER_OPTIONS_H_
