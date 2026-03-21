#ifndef TELEPATH_IO_DISK_BACKEND_H_
#define TELEPATH_IO_DISK_BACKEND_H_

#include <cstddef>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

class DiskBackend {
 public:
  virtual ~DiskBackend() = default;

  virtual Status ReadBlock(const BufferTag &tag, std::byte *out,
                           std::size_t size) = 0;
  virtual Status WriteBlock(const BufferTag &tag, const std::byte *data,
                            std::size_t size) = 0;
};

}  // namespace telepath

#endif  // TELEPATH_IO_DISK_BACKEND_H_
