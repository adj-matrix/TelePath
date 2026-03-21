#ifndef TELEPATH_IO_POSIX_DISK_BACKEND_H_
#define TELEPATH_IO_POSIX_DISK_BACKEND_H_

#include <string>

#include "telepath/io/disk_backend.h"

namespace telepath {

class PosixDiskBackend : public DiskBackend {
 public:
  explicit PosixDiskBackend(std::string root_path, std::size_t page_size);

  Status ReadBlock(const BufferTag &tag, std::byte *out,
                   std::size_t size) override;
  Status WriteBlock(const BufferTag &tag, const std::byte *data,
                    std::size_t size) override;

 private:
  Result<int> OpenFile(FileId file_id, int flags) const;
  std::string BuildPath(FileId file_id) const;

  std::string root_path_;
  std::size_t page_size_{0};
};

}  // namespace telepath

#endif  // TELEPATH_IO_POSIX_DISK_BACKEND_H_
