#ifndef TELEPATH_LIB_BUFFER_SHARED_SEGMENT_H_
#define TELEPATH_LIB_BUFFER_SHARED_SEGMENT_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/types.h>
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
    std::uint32_t crash_recovery{0};
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

  enum class CrashCheck {
    kNone,
    // This call found a process that died attached and marked the segment
    // crashed; the caller recovers what it can.
    kDetected,
    // The segment was already marked crashed.
    kObserved,
  };

  // Checks whether an attached process died without detaching. A process
  // holds a lock on its slot's byte of the segment object while attached, and
  // the kernel releases it when the process dies, so a slot with a pid whose
  // lock is free belongs to a dead process. The process that marks the
  // segment crashed also removes its name, so new managers create a fresh
  // segment; a crashed segment never removes the name again.
  // With kDetected, `dead_slots` receives one bit per dead process's slot.
  auto CheckForCrash(std::uint64_t *dead_slots = nullptr) -> CrashCheck;

  // Nonzero once the segment is marked crashed.
  auto crashed_word() const -> const std::atomic<std::uint32_t> *;

  // This process's slot; holder_slot in descriptors stores it plus one.
  auto slot() const -> int { return slot_; }

  // Whether this process runs the background cleaner for every attached
  // process. The first caller to ask becomes the leader and stays it until it
  // detaches or dies; the lock behind it is released by the kernel when the
  // process exits, so a later caller then takes over.
  bool TryLeadCleaner();

 private:
  struct Header;

  SharedSegment(std::string name, bool remove_when_unused, int fd, std::byte *base, std::size_t bytes, bool created);

  // Returns null when the segment was being removed, or vanished between
  // the attempt to create it and the attempt to attach, and opening must retry.
  static auto OpenOnce(const SharedMemoryOptions &options, const Shape &shape) -> Result<std::unique_ptr<SharedSegment>>;

  auto header() const -> Header &;
  auto ClaimProcessSlot() -> Status;
  bool LockByte(off_t byte, short type) const;
  void RemoveNameIfStillOurs() const;

  std::string name_;
  bool remove_when_unused_;
  // Kept open for the cleaner lock, which lives as long as this descriptor.
  int fd_;
  std::byte *base_;
  std::size_t bytes_;
  bool created_;
  bool counted_{false};
  bool leading_cleaner_{false};
  int slot_{-1};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_SHARED_SEGMENT_H_
