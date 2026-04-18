#ifndef TELEPATH_IO_DISK_BACKEND_FACTORY_H_
#define TELEPATH_IO_DISK_BACKEND_FACTORY_H_

#include <memory>
#include <string>

#include "telepath/common/status.h"
#include "telepath/io/disk_backend.h"
#include "telepath/io/disk_backend_options.h"

namespace telepath {

// Returns whether the current build can instantiate an io_uring backend.
bool IsIoUringBackendSupported();

// Creates a disk backend using the requested policy, falling back when
// permitted by `options`.
auto CreateDiskBackend(
  std::string root_path,
  std::size_t page_size,
  const DiskBackendOptions &options = {}
) -> Result<std::unique_ptr<DiskBackend>>;

}  // namespace telepath

#endif  // TELEPATH_IO_DISK_BACKEND_FACTORY_H_
