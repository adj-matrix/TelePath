#include "file_descriptor_cache.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace telepath {

FileDescriptorCache::FileDescriptorCache(std::string root_path, std::size_t max_open_files)
  : root_path_(std::move(root_path)), max_open_files_(max_open_files == 0 ? 64 : max_open_files) {}

FileDescriptorCache::~FileDescriptorCache() {
  std::lock_guard<std::mutex> guard(latch_);
  for (const auto &[file_id, entry] : entries_) {
    (void)file_id;
    if (entry.fd >= 0) close(entry.fd);
  }
}

auto FileDescriptorCache::Acquire(FileId file_id) -> Result<int> {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(file_id);
  if (it != entries_.end()) {
    ++it->second.pins;
    it->second.last_used = ++clock_;
    return it->second.fd;
  }

  EvictIdleLocked(max_open_files_ - 1);
  const std::string path = root_path_ + "/file_" + std::to_string(file_id) + ".tp";
  const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return IoError("open failed", errno);
  entries_.emplace(file_id, Entry{fd, ++clock_, 1});
  return fd;
}

void FileDescriptorCache::Release(FileId file_id) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(file_id);
  if (it == entries_.end()) return;
  if (it->second.pins > 0) --it->second.pins;
  it->second.last_used = ++clock_;
  EvictIdleLocked(max_open_files_);
}

auto FileDescriptorCache::FileSize(FileId file_id) -> Result<std::uint64_t> {
  Result<int> fd = Acquire(file_id);
  if (!fd.has_value()) return PropagateError(fd);
  struct stat info{};
  const int result = fstat(fd.value(), &info);
  const int error = errno;
  Release(file_id);
  if (result != 0) return IoError("fstat failed", error);
  return static_cast<std::uint64_t>(info.st_size);
}

auto FileDescriptorCache::SetFileSize(FileId file_id, std::uint64_t size) -> Status {
  Result<int> fd = Acquire(file_id);
  if (!fd.has_value()) return PropagateError(fd);
  const int result = ftruncate(fd.value(), static_cast<off_t>(size));
  const int error = errno;
  Release(file_id);
  if (result != 0) return IoError("ftruncate failed", error);
  return Ok();
}

void FileDescriptorCache::CloseIdle() {
  std::lock_guard<std::mutex> guard(latch_);
  EvictIdleLocked(0);
}

void FileDescriptorCache::EvictIdleLocked(std::size_t target_size) {
  while (entries_.size() > target_size) {
    auto victim = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->second.pins != 0) continue;
      if (victim == entries_.end() || it->second.last_used < victim->second.last_used) victim = it;
    }
    if (victim == entries_.end()) return;
    if (victim->second.fd >= 0) close(victim->second.fd);
    entries_.erase(victim);
  }
}

}  // namespace telepath
