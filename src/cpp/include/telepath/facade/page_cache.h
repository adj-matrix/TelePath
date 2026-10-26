#ifndef TELEPATH_FACADE_PAGE_CACHE_H_
#define TELEPATH_FACADE_PAGE_CACHE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <unordered_map>

#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "telepath/io/disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

class BufferManager;
class TelemetrySink;

struct PageCacheOptions {
  // Kernel configuration: pool size, block size, writeback, and so on.
  BufferManagerOptions buffer{};
  // Largest readahead window in blocks; 0 disables readahead. The window
  // starts at 4 blocks on the second sequential read of a file and doubles on
  // each further one.
  std::size_t max_readahead_blocks{32};
  // Writes dirty pages back in the background before they must be evicted
  // (sets buffer.enable_background_cleaner).
  bool write_behind{true};
};

// A byte-addressed cache of files, built on one BufferManager that it owns.
// Each file has a logical size: reads stop at it and writes extend it.
// Sequential reads trigger readahead, and dirty pages are written back in the
// background. Data and sizes become durable through Sync() or Close(); the
// backing file may keep a block-rounded size until then. All methods are
// thread-safe. Concurrent writes to overlapping ranges are applied block by
// block with no atomicity across blocks, and Truncate() must not race with
// reads or writes of the same file.
class PageCache {
 public:
  // Creates the cache and its kernel. `backend` must implement
  // DiskBackend::GetFileSize() and SetFileSize(). A null `replacer` selects
  // clock-sweep; a null `telemetry_sink` selects the no-op sink.
  static auto Create(
    PageCacheOptions options,
    std::unique_ptr<DiskBackend> backend,
    std::unique_ptr<Replacer> replacer = nullptr,
    std::shared_ptr<TelemetrySink> telemetry_sink = nullptr) -> Result<std::unique_ptr<PageCache>>;

  PageCache(const PageCache &) = delete;
  PageCache &operator=(const PageCache &) = delete;
  ~PageCache();

  // Copies up to `out.size()` bytes starting at `offset` and returns how many
  // were copied: fewer at the end of the file, and 0 at or past it.
  auto Read(FileId file_id, std::uint64_t offset, std::span<std::byte> out) -> Result<std::size_t>;
  // Copies `data` to `offset`, extending the file when it ends past the
  // current size; a gap between the old size and `offset` reads as zeros. On
  // an error a prefix of `data` may have been written, as with a short POSIX
  // write, and the size then covers that prefix.
  auto Write(FileId file_id, std::uint64_t offset, std::span<const std::byte> data) -> Status;
  auto FileSize(FileId file_id) -> Result<std::uint64_t>;
  // Sets the file's size. Shrinking discards cached data past the new size,
  // and a later extension reads as zeros.
  auto Truncate(FileId file_id, std::uint64_t size) -> Status;
  // Writes back the file's dirty pages, sets its backing size, and makes both
  // durable.
  auto Sync(FileId file_id) -> Status;
  // Syncs every file this cache has changed and closes it; afterwards every
  // call returns ErrorCode::kUnavailable. When a sync fails, returns its error
  // without closing, so the caller can retry. Stop other threads' calls first.
  auto Close() -> Status;

  auto block_size() const -> std::size_t { return block_size_; }

 private:
  struct FileState;

  PageCache(PageCacheOptions options, DiskBackend *backend, std::unique_ptr<BufferManager> manager);

  auto State(FileId file_id) -> Result<FileState *>;
  void Readahead(FileId file_id, FileState &state, std::uint64_t offset, std::size_t length, std::uint64_t file_size);
  auto SyncState(FileId file_id, FileState &state) -> Status;

  PageCacheOptions options_;
  std::size_t block_size_;
  std::atomic<bool> closed_{false};
  DiskBackend *backend_;
  std::unique_ptr<BufferManager> manager_;
  std::shared_mutex files_latch_;
  std::unordered_map<FileId, std::unique_ptr<FileState>> files_;
};

}  // namespace telepath

#endif  // TELEPATH_FACADE_PAGE_CACHE_H_
