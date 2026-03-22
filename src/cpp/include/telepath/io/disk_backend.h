#ifndef TELEPATH_IO_DISK_BACKEND_H_
#define TELEPATH_IO_DISK_BACKEND_H_

#include <cstddef>
#include <cstdint>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

enum class DiskOperation {
  kRead = 0,
  kWrite,
};

struct DiskRequest {
  uint64_t request_id{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  std::byte *mutable_buffer{nullptr};
  const std::byte *const_buffer{nullptr};
  std::size_t size{0};
};

struct DiskCompletion {
  uint64_t request_id{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  Status status{};
};

class DiskBackend {
 public:
  virtual ~DiskBackend() = default;

  virtual Result<uint64_t> SubmitRead(const BufferTag &tag, std::byte *out,
                                      std::size_t size) = 0;
  virtual Result<uint64_t> SubmitWrite(const BufferTag &tag,
                                       const std::byte *data,
                                       std::size_t size) = 0;
  virtual Result<DiskCompletion> PollCompletion() = 0;
};

}  // namespace telepath

#endif  // TELEPATH_IO_DISK_BACKEND_H_
