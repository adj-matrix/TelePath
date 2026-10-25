#ifndef TELEPATH_IO_DISK_BACKEND_OPTIONS_H_
#define TELEPATH_IO_DISK_BACKEND_OPTIONS_H_

#include <cstddef>

namespace telepath {

enum class DiskBackendKind {
  kAuto = 0,
  kPosix,
  kIoUring,
};

struct DiskBackendOptions {
  DiskBackendKind preferred_kind{DiskBackendKind::kAuto};
  bool allow_fallback{true};
  std::size_t queue_depth{0};
  // Maximum number of data-file descriptors a backend should keep open.
  // Set to zero to derive a conservative default.
  std::size_t max_open_files{0};
  // When true, each completed page write is followed by a durability sync.
  // Benchmark runs may disable this to measure buffer-pool behavior without
  // forcing a device-cache flush for every 4 KiB write.
  bool sync_on_write{true};
  // Threads the POSIX backend uses for asynchronous requests (prefetch and
  // background writeback). Set to zero to derive a default.
  std::size_t io_worker_count{0};

  auto ResolveQueueDepth() const -> std::size_t {
    if (queue_depth != 0) return queue_depth;

    switch (preferred_kind) {
    case DiskBackendKind::kIoUring:
      return 64;
    case DiskBackendKind::kAuto:
      return 32;
    case DiskBackendKind::kPosix:
      return 1;
    }

    return 1;
  }

  auto ResolveMaxOpenFiles() const -> std::size_t {
    if (max_open_files != 0) return max_open_files;
    return 64;
  }

  auto ResolveIoWorkerCount() const -> std::size_t {
    if (io_worker_count != 0) return io_worker_count;
    return 4;
  }
};

}  // namespace telepath

#endif  // TELEPATH_IO_DISK_BACKEND_OPTIONS_H_
