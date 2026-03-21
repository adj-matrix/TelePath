#include "telepath/io/posix_disk_backend.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace telepath {

namespace {

std::string BuildErrnoMessage(const std::string &prefix) {
  return prefix + ": " + std::strerror(errno);
}

}  // namespace

PosixDiskBackend::PosixDiskBackend(std::string root_path, std::size_t page_size)
    : root_path_(std::move(root_path)), page_size_(page_size) {}

Status PosixDiskBackend::ReadBlock(const BufferTag &tag, std::byte *out,
                                   std::size_t size) {
  if (size != page_size_) {
    return Status::InvalidArgument("read size does not match page size");
  }

  Result<int> fd_result = OpenFile(tag.file_id, O_RDWR | O_CREAT);
  if (!fd_result.ok()) {
    return fd_result.status();
  }
  const int fd = fd_result.value();

  const off_t offset = static_cast<off_t>(tag.block_id * page_size_);
  const ssize_t bytes_read = pread(fd, out, size, offset);
  if (bytes_read < 0) {
    close(fd);
    return Status::IoError(BuildErrnoMessage("pread failed"));
  }
  if (bytes_read < static_cast<ssize_t>(size)) {
    std::memset(out + bytes_read, 0, size - bytes_read);
  }

  close(fd);
  return Status::Ok();
}

Status PosixDiskBackend::WriteBlock(const BufferTag &tag, const std::byte *data,
                                    std::size_t size) {
  if (size != page_size_) {
    return Status::InvalidArgument("write size does not match page size");
  }

  Result<int> fd_result = OpenFile(tag.file_id, O_RDWR | O_CREAT);
  if (!fd_result.ok()) {
    return fd_result.status();
  }
  const int fd = fd_result.value();

  const off_t offset = static_cast<off_t>(tag.block_id * page_size_);
  const ssize_t bytes_written = pwrite(fd, data, size, offset);
  if (bytes_written != static_cast<ssize_t>(size)) {
    close(fd);
    if (bytes_written < 0) {
      return Status::IoError(BuildErrnoMessage("pwrite failed"));
    }
    return Status::IoError("partial page write");
  }

  if (fsync(fd) != 0) {
    close(fd);
    return Status::IoError(BuildErrnoMessage("fsync failed"));
  }

  close(fd);
  return Status::Ok();
}

Result<int> PosixDiskBackend::OpenFile(FileId file_id, int flags) const {
  const std::string path = BuildPath(file_id);
  const int fd = open(path.c_str(), flags, 0644);
  if (fd < 0) {
    return Status::IoError(BuildErrnoMessage("open failed"));
  }
  return fd;
}

std::string PosixDiskBackend::BuildPath(FileId file_id) const {
  return root_path_ + "/file_" + std::to_string(file_id) + ".tp";
}

}  // namespace telepath
