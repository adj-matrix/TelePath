#ifndef TELEPATH_LIB_IO_FILE_DESCRIPTOR_CACHE_H_
#define TELEPATH_LIB_IO_FILE_DESCRIPTOR_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

// Thread-safe cache of open data-file descriptors. Acquire() pins a descriptor
// until the matching Release(); only unpinned descriptors are closed when the
// cache exceeds its capacity.
class FileDescriptorCache {
 public:
  FileDescriptorCache(std::string root_path, std::size_t max_open_files);
  ~FileDescriptorCache();

  FileDescriptorCache(const FileDescriptorCache &) = delete;
  FileDescriptorCache &operator=(const FileDescriptorCache &) = delete;

  auto Acquire(FileId file_id) -> Result<int>;
  void Release(FileId file_id);
  void CloseIdle();

 private:
  struct Entry {
    int fd{-1};
    uint64_t last_used{0};
    std::size_t pins{0};
  };

  void EvictIdleLocked(std::size_t target_size);

  std::string root_path_;
  std::size_t max_open_files_;
  std::mutex latch_;
  uint64_t clock_{0};
  std::unordered_map<FileId, Entry> entries_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_IO_FILE_DESCRIPTOR_CACHE_H_
