#include "shared_segment.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <thread>
#include <utility>

namespace telepath {

namespace {

constexpr std::uint64_t kMagic = 0x3147455354504c54ULL;  // "TLPTSEG1"
constexpr std::uint32_t kLayoutVersion = 1;
constexpr std::uint32_t kInitializing = 0;
constexpr std::uint32_t kReady = 1;
// Attached count of a segment whose last user is removing its name.
constexpr std::uint32_t kRemoving = UINT32_MAX;
constexpr std::size_t kMaxProcesses = 64;
constexpr std::size_t kMaxNamespaces = 64;
constexpr std::size_t kReplacerKindBytes = 96;
constexpr std::size_t kPageSize = 4096;
constexpr auto kAttachTimeout = std::chrono::seconds(10);

auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t { return (value + alignment - 1) & ~(alignment - 1); }

// Start time of this process in clock ticks since boot, which tells a reused
// pid apart from the original process.
auto ProcessStartTime() -> std::uint64_t {
  std::ifstream stat("/proc/self/stat");
  std::string line;
  std::getline(stat, line);
  const std::size_t close = line.rfind(')');
  if (close == std::string::npos) return 0;
  std::istringstream fields(line.substr(close + 2));
  std::string field;
  for (int index = 3; index <= 22 && fields >> field; ++index) {
    if (index == 22) return std::stoull(field);
  }
  return 0;
}

}  // namespace

struct SharedSegment::Header {
  struct ProcessSlot {
    std::atomic<std::uint32_t> pid{0};
    std::uint32_t reserved{0};
    std::uint64_t start_time{0};
  };

  // Reserved for shared namespaces (kernel-refactor.md, section 8).
  struct NamespaceEntry {
    std::array<std::byte, 64> reserved{};
  };

  std::uint64_t magic{kMagic};
  std::uint32_t layout_version{kLayoutVersion};
  std::atomic<std::uint32_t> state{kInitializing};
  std::uint64_t segment_bytes{0};
  std::uint64_t block_size{0};
  std::uint64_t pool_size{0};
  std::uint64_t stripe_count{0};
  std::uint64_t replacer_state_bytes{0};
  std::uint64_t kernel_bytes{0};
  std::array<char, kReplacerKindBytes> replacer_kind{};
  std::uint64_t replacer_offset{0};
  std::uint64_t kernel_offset{0};
  std::atomic<std::uint32_t> attached{0};
  // Reserved for electing the process that runs background writeback.
  std::atomic<std::uint32_t> cleaner_leader{0};
  std::array<ProcessSlot, kMaxProcesses> processes{};
  std::array<NamespaceEntry, kMaxNamespaces> namespaces{};
  SharedWrittenFiles written_files{};
};

auto SharedSegment::Open(const SharedMemoryOptions &options, const Shape &shape) -> Result<std::unique_ptr<SharedSegment>> {
  if (options.name.empty()) return InvalidArgument("shared memory name must not be empty");
  const auto deadline = std::chrono::steady_clock::now() + kAttachTimeout;
  while (true) {
    Result<std::unique_ptr<SharedSegment>> segment = OpenOnce(options, shape);
    if (!segment.has_value() || segment.value() != nullptr) return segment;
    if (std::chrono::steady_clock::now() >= deadline) return Unavailable("shared memory segment is stuck being removed");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

auto SharedSegment::OpenOnce(const SharedMemoryOptions &options, const Shape &shape) -> Result<std::unique_ptr<SharedSegment>> {
  const std::size_t replacer_offset = AlignUp(sizeof(Header), kCacheLineSize);
  const std::size_t kernel_offset = AlignUp(replacer_offset + shape.replacer_state_bytes, kPageSize);
  const std::size_t total = kernel_offset + shape.kernel_bytes;

  bool created = false;
  int fd = -1;
  if (options.mode != SharedMemoryMode::kAttach) {
    fd = shm_open(options.name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) created = true;
    else if (errno != EEXIST) return IoError("shm_open failed", errno);
    else if (options.mode == SharedMemoryMode::kCreate) return InvalidArgument("shared memory segment already exists");
  }
  if (!created) {
    fd = shm_open(options.name.c_str(), O_RDWR, 0);
    if (fd < 0) return errno == ENOENT ? NotFound("shared memory segment does not exist") : IoError("shm_open failed", errno);
  }

  std::size_t mapped_bytes = total;
  if (created) {
    if (ftruncate(fd, static_cast<off_t>(total)) != 0) {
      const int error = errno;
      close(fd);
      shm_unlink(options.name.c_str());
      return IoError("failed to size shared memory segment", error);
    }
  } else {
    // The creator sizes the segment right after creating it.
    const auto deadline = std::chrono::steady_clock::now() + kAttachTimeout;
    struct stat info{};
    while (fstat(fd, &info) == 0 && static_cast<std::size_t>(info.st_size) < sizeof(Header) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (static_cast<std::size_t>(info.st_size) < sizeof(Header)) {
      close(fd);
      return Unavailable("shared memory segment was never initialized");
    }
    mapped_bytes = static_cast<std::size_t>(info.st_size);
  }

  void *address = mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  const int map_error = errno;
  close(fd);
  if (address == MAP_FAILED) {
    if (created) shm_unlink(options.name.c_str());
    return IoError("failed to map shared memory segment", map_error);
  }

  std::unique_ptr<SharedSegment> segment(new SharedSegment(options.name, options.remove_when_unused, static_cast<std::byte *>(address), mapped_bytes, created));
  Header &header = segment->header();
  if (created) {
    std::construct_at(&header);
    header.segment_bytes = total;
    header.block_size = shape.block_size;
    header.pool_size = shape.pool_size;
    header.stripe_count = shape.stripe_count;
    header.replacer_state_bytes = shape.replacer_state_bytes;
    header.kernel_bytes = shape.kernel_bytes;
    std::strncpy(header.replacer_kind.data(), shape.replacer_kind.c_str(), kReplacerKindBytes - 1);
    header.replacer_offset = replacer_offset;
    header.kernel_offset = kernel_offset;
  } else {
    const auto deadline = std::chrono::steady_clock::now() + kAttachTimeout;
    while (header.state.load(std::memory_order_acquire) != kReady) {
      if (std::chrono::steady_clock::now() >= deadline) return Unavailable("shared memory segment was never initialized");
      sync::FutexWaitFor(header.state, kInitializing, std::chrono::milliseconds(10));
    }
    if (header.magic != kMagic || header.layout_version != kLayoutVersion) return InvalidArgument("shared memory segment has an unknown layout");
    std::array<char, kReplacerKindBytes> kind{};
    std::strncpy(kind.data(), shape.replacer_kind.c_str(), kReplacerKindBytes - 1);
    const bool same_shape = header.segment_bytes == total && header.segment_bytes == mapped_bytes
      && header.block_size == shape.block_size && header.pool_size == shape.pool_size
      && header.stripe_count == shape.stripe_count && header.replacer_state_bytes == shape.replacer_state_bytes
      && header.kernel_bytes == shape.kernel_bytes && header.replacer_kind == kind;
    if (!same_shape) return InvalidArgument("shared memory segment was created with a different configuration");
  }
  std::uint32_t attached = header.attached.load(std::memory_order_acquire);
  do {
    // The name is about to disappear; open whatever replaces it.
    if (attached == kRemoving) return std::unique_ptr<SharedSegment>();
  } while (!header.attached.compare_exchange_weak(attached, attached + 1, std::memory_order_acq_rel));
  segment->counted_ = true;
  if (Status claimed = segment->ClaimProcessSlot(); !claimed.has_value()) return PropagateError(claimed);
  return segment;
}

auto SharedSegment::Remove(const std::string &name) -> Status {
  if (shm_unlink(name.c_str()) == 0) return Ok();
  if (errno == ENOENT) return NotFound("shared memory segment does not exist");
  return IoError("shm_unlink failed", errno);
}

SharedSegment::SharedSegment(std::string name, bool remove_when_unused, std::byte *base, std::size_t bytes, bool created)
  : name_(std::move(name)), remove_when_unused_(remove_when_unused), base_(base), bytes_(bytes), created_(created) {}

SharedSegment::~SharedSegment() {
  Header &header = this->header();
  if (slot_ >= 0) header.processes[slot_].pid.store(0, std::memory_order_release);
  const bool never_ready = created_ && header.state.load(std::memory_order_acquire) != kReady;
  bool remove = never_ready;
  if (counted_ && header.attached.fetch_sub(1, std::memory_order_acq_rel) == 1 && remove_when_unused_) {
    std::uint32_t expected = 0;
    remove = header.attached.compare_exchange_strong(expected, kRemoving, std::memory_order_acq_rel);
  }
  // A creator that failed before finishing must not leave a segment that
  // attachers would wait on forever.
  if (remove) shm_unlink(name_.c_str());
  munmap(base_, bytes_);
}

void SharedSegment::MarkReady() {
  Header &header = this->header();
  header.state.store(kReady, std::memory_order_release);
  sync::FutexWakeAll(header.state);
}

auto SharedSegment::kernel_base() const -> std::byte * { return base_ + header().kernel_offset; }

auto SharedSegment::replacer_state() const -> std::span<std::byte> {
  return std::span<std::byte>(base_ + header().replacer_offset, header().replacer_state_bytes);
}

auto SharedSegment::written_files() const -> SharedWrittenFiles & { return header().written_files; }

auto SharedSegment::header() const -> Header & { return *std::launder(reinterpret_cast<Header *>(base_)); }

auto SharedSegment::ClaimProcessSlot() -> Status {
  Header &header = this->header();
  const auto pid = static_cast<std::uint32_t>(getpid());
  for (std::size_t index = 0; index < kMaxProcesses; ++index) {
    std::uint32_t expected = 0;
    if (!header.processes[index].pid.compare_exchange_strong(expected, pid, std::memory_order_acq_rel)) continue;
    header.processes[index].start_time = ProcessStartTime();
    slot_ = static_cast<int>(index);
    return Ok();
  }
  return ResourceExhausted("shared memory segment has no free process slot");
}

}  // namespace telepath
