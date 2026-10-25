#ifndef TELEPATH_LIB_IO_DISK_REQUEST_UTIL_H_
#define TELEPATH_LIB_IO_DISK_REQUEST_UTIL_H_

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "telepath/io/disk_backend.h"

namespace telepath {

inline auto ValidateDiskRequest(const DiskRequest &request, std::size_t max_request_blocks) -> Status {
  if (request.block_count == 0 || request.block_count > max_request_blocks) return InvalidArgument("disk request block count is out of range");
  for (std::uint32_t i = 0; i < request.block_count; ++i) {
    if (request.buffers[i] == nullptr) return InvalidArgument("disk request buffer must not be null");
  }
  return Ok();
}

// Zero-fills the bytes of a read request past the first `transferred` bytes.
inline void ZeroFillReadTail(const DiskRequest &request, std::size_t block_size, std::size_t transferred) {
  for (std::uint32_t i = 0; i < request.block_count; ++i) {
    const std::size_t block_begin = i * block_size;
    if (transferred >= block_begin + block_size) continue;
    const std::size_t offset = transferred > block_begin ? transferred - block_begin : 0;
    std::memset(request.buffers[i] + offset, 0, block_size - offset);
  }
}

}  // namespace telepath

#endif  // TELEPATH_LIB_IO_DISK_REQUEST_UTIL_H_
