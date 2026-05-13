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
};

}  // namespace telepath

#endif  // TELEPATH_IO_DISK_BACKEND_OPTIONS_H_
