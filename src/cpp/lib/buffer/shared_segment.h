#ifndef TELEPATH_LIB_BUFFER_SHARED_SEGMENT_H_
#define TELEPATH_LIB_BUFFER_SHARED_SEGMENT_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "telepath/options/buffer_manager_options.h"
#include "../sync/futex.h"

namespace telepath {

// Files written since their last sync, for every process attached to the
// segment. A file that does not fit is remembered by the writing process only.
struct SharedWrittenFiles {
  static constexpr std::size_t kCapacity = 1024;

  sync::FutexMutex latch;
  std::uint32_t count{0};
  std::array<FileId, kCapacity> files{};
};

// A POSIX shared-memory segment holding a header, the replacer state, and the
// kernel region, mapped by every process attached to one cache.
class SharedSegment {
 public:
  // What every attached process must agree on.
  struct Shape {
    std::uint64_t block_size{0};
    std::uint64_t pool_size{0};
    std::uint64_t stripe_count{0};
    std::uint64_t replacer_state_bytes{0};
    std::string replacer_kind;
    std::uint64_t kernel_bytes{0};
  };

  // Creates or attaches to the segment named in `options`. An attaching
  // process waits until the creator finished initializing and validates the
  // shape.
  static auto Open(const SharedMemoryOptions &options, const Shape &shape) -> Result<std::unique_ptr<SharedSegment>>;
  static auto Remove(const std::string &name) -> Status;

  SharedSegment(const SharedSegment &) = delete;
  SharedSegment &operator=(const SharedSegment &) = delete;
  ~SharedSegment();

  // Whether this process created the segment and must initialize its
  // contents before MarkReady().
  bool created() const { return created_; }
  void MarkReady();

  auto kernel_base() const -> std::byte *;
  auto replacer_state() const -> std::span<std::byte>;
  auto written_files() const -> SharedWrittenFiles &;

 private:
  struct Header;

  SharedSegment(std::string name, bool remove_when_unused, std::byte *base, std::size_t bytes, bool created);

  // Returns null when the segment was being removed and opening must retry.
  static auto OpenOnce(const SharedMemoryOptions &options, const Shape &shape) -> Result<std::unique_ptr<SharedSegment>>;

  auto header() const -> Header &;
  auto ClaimProcessSlot() -> Status;

  std::string name_;
  bool remove_when_unused_;
  std::byte *base_;
  std::size_t bytes_;
  bool created_;
  bool counted_{false};
  int slot_{-1};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_SHARED_SEGMENT_H_
