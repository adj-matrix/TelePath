#include "observation_publisher.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <utility>

namespace telepath {

namespace {

using observation::ObservationFrameRecord;
using observation::ObservationHeader;
using observation::ObservationProcessRecord;

// Byte locks on the object, released when the process dies: every user
// read-locks kUsersByte, so whoever can write-lock it is the last user; the
// frame publisher write-locks kPublisherByte; opening and removing the name
// write-lock kSetupByte briefly, so a name is never removed between another
// process opening it and read-locking kUsersByte.
constexpr off_t kUsersByte = 0;
constexpr off_t kPublisherByte = 1;
constexpr off_t kSetupByte = 2;

auto LockByte(int fd, off_t byte, short type, bool wait) -> bool {
  struct flock lock{};
  lock.l_type = type;
  lock.l_whence = SEEK_SET;
  lock.l_start = byte;
  lock.l_len = 1;
  while (fcntl(fd, wait ? F_OFD_SETLKW : F_OFD_SETLK, &lock) != 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

auto UnixMillis() -> std::uint64_t {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

auto ObjectBytes(std::size_t pool_size) -> std::size_t {
  return sizeof(ObservationHeader) + observation::kObservationProcessCapacity * sizeof(ObservationProcessRecord) + pool_size * sizeof(ObservationFrameRecord);
}

void BeginWrite(std::uint64_t &sequence) {
  std::atomic_ref<std::uint64_t>(sequence).fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
}

void EndWrite(std::uint64_t &sequence) { std::atomic_ref<std::uint64_t>(sequence).fetch_add(1, std::memory_order_release); }

}  // namespace

auto ObservationPublisher::Open(const ObservationOptions &options, std::size_t pool_size, std::size_t page_size, bool cross_process) -> Result<std::unique_ptr<ObservationPublisher>> {
  const std::string &name = options.shared_memory_name;
  if (name.size() < 2 || name.front() != '/' || name.find('/', 1) != std::string::npos) return InvalidArgument("observation shared-memory name must look like \"/name\"");
  if (options.publish_interval.count() <= 0) return InvalidArgument("observation publish interval must be positive");
  const std::size_t bytes = ObjectBytes(pool_size);

  int fd = -1;
  while (true) {
    fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return IoError("failed to open observation shared memory", errno);
    if (!LockByte(fd, kSetupByte, F_WRLCK, true)) {
      const int error = errno;
      close(fd);
      return IoError("failed to lock observation shared memory", error);
    }
    struct stat info{};
    if (fstat(fd, &info) != 0) {
      const int error = errno;
      close(fd);
      return IoError("failed to inspect observation shared memory", error);
    }
    // The last user removed the name between our open and our lock.
    if (info.st_nlink == 0) {
      close(fd);
      continue;
    }
    if (info.st_size == 0 && ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
      const int error = errno;
      close(fd);
      return IoError("failed to size observation shared memory", error);
    }
    if (info.st_size != 0 && static_cast<std::size_t>(info.st_size) != bytes) {
      close(fd);
      return InvalidArgument("observation shared memory belongs to a cache with a different pool size");
    }
    break;
  }

  void *address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (address == MAP_FAILED) {
    const int error = errno;
    close(fd);
    return IoError("failed to map observation shared memory", error);
  }
  std::unique_ptr<ObservationPublisher> publisher(new ObservationPublisher(name, options.publish_interval, fd, static_cast<std::byte *>(address), bytes));
  ObservationHeader &header = publisher->header();
  if (header.magic == 0) {
    header.version = observation::kObservationVersion;
    header.header_bytes = sizeof(ObservationHeader);
    header.frame_record_bytes = sizeof(ObservationFrameRecord);
    header.pool_size = pool_size;
    header.page_size = page_size;
    header.processes_offset = sizeof(ObservationHeader);
    header.frames_offset = sizeof(ObservationHeader) + observation::kObservationProcessCapacity * sizeof(ObservationProcessRecord);
    header.process_record_bytes = sizeof(ObservationProcessRecord);
    header.process_capacity = observation::kObservationProcessCapacity;
    header.cross_process = cross_process ? 1 : 0;
    std::atomic_ref<std::uint32_t>(header.magic).store(observation::kObservationMagic, std::memory_order_release);
  } else if (header.magic != observation::kObservationMagic || header.version != observation::kObservationVersion) {
    return InvalidArgument("observation shared memory has an unknown format");
  } else if (header.pool_size != pool_size || header.page_size != page_size || header.cross_process != (cross_process ? 1 : 0)) {
    return InvalidArgument("observation shared memory belongs to a different cache");
  }
  if (Status claimed = publisher->ClaimProcessRecord(); !claimed.has_value()) return PropagateError(claimed);
  if (!LockByte(fd, kUsersByte, F_RDLCK, false)) return IoError("failed to lock observation shared memory", errno);
  LockByte(fd, kSetupByte, F_UNLCK, false);
  return publisher;
}

ObservationPublisher::ObservationPublisher(std::string name, std::chrono::milliseconds interval, int fd, std::byte *base, std::size_t bytes)
  : name_(std::move(name)), interval_(interval), fd_(fd), base_(base), bytes_(bytes) {}

ObservationPublisher::~ObservationPublisher() {
  Stop();
  if (record_ != nullptr) std::atomic_ref<std::uint32_t>(record_->pid).store(0, std::memory_order_release);
  munmap(base_, bytes_);
  if (LockByte(fd_, kSetupByte, F_WRLCK, true) && LockByte(fd_, kUsersByte, F_WRLCK, false)) shm_unlink(name_.c_str());
  close(fd_);
}

void ObservationPublisher::Start(Sources sources) {
  sources_ = std::move(sources);
  thread_ = std::thread([this] { Run(); });
}

void ObservationPublisher::Stop() {
  {
    std::lock_guard<std::mutex> guard(stop_latch_);
    stopping_ = true;
  }
  stop_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void ObservationPublisher::Run() {
  std::unique_lock<std::mutex> lock(stop_latch_);
  while (!stop_cv_.wait_for(lock, interval_, [this] { return stopping_; })) {
    lock.unlock();
    Publish();
    lock.lock();
  }
}

void ObservationPublisher::Publish() {
  std::lock_guard<std::mutex> guard(publish_latch_);
  if (!sources_.frames) return;
  const std::uint64_t now = UnixMillis();
  const TelemetrySnapshot counters = sources_.counters();
  BeginWrite(record_->sequence);
  record_->updated_unix_ms = now;
  record_->buffer_hits = counters.buffer_hits;
  record_->buffer_misses = counters.buffer_misses;
  record_->disk_reads = counters.disk_reads;
  record_->disk_writes = counters.disk_writes;
  record_->evictions = counters.evictions;
  record_->dirty_flushes = counters.dirty_flushes;
  record_->flush_tasks_scheduled = counters.flush_tasks_scheduled;
  record_->flush_tasks_completed = counters.flush_tasks_completed;
  record_->flush_failures = counters.flush_failures;
  record_->cleaner_flushes_scheduled = counters.cleaner_flushes_scheduled;
  record_->cleaner_flushes_finished = counters.cleaner_flushes_finished;
  record_->cleaner_flushes_skipped = counters.cleaner_flushes_skipped;
  record_->eviction_failures = counters.eviction_failures;
  EndWrite(record_->sequence);

  if (!TryBecomePublisher()) return;
  ObservationHeader &header = this->header();
  BeginWrite(header.sequence);
  header.published_unix_ms = now;
  header.publisher_pid = static_cast<std::uint32_t>(getpid());
  sources_.frames(&header.summary, &header.min_rec_lsn, frames());
  EndWrite(header.sequence);
}

auto ObservationPublisher::header() const -> ObservationHeader & { return *reinterpret_cast<ObservationHeader *>(base_); }

auto ObservationPublisher::frames() const -> std::span<ObservationFrameRecord> {
  return std::span<ObservationFrameRecord>(reinterpret_cast<ObservationFrameRecord *>(base_ + header().frames_offset), header().pool_size);
}

auto ObservationPublisher::ClaimProcessRecord() -> Status {
  auto *records = reinterpret_cast<ObservationProcessRecord *>(base_ + header().processes_offset);
  const auto pid = static_cast<std::uint32_t>(getpid());
  // Records of processes that exited without cleaning up are reused last.
  for (bool reclaim : {false, true}) {
    for (std::size_t index = 0; index < observation::kObservationProcessCapacity; ++index) {
      std::atomic_ref<std::uint32_t> owner(records[index].pid);
      std::uint32_t expected = owner.load(std::memory_order_acquire);
      if (expected != 0 && !(reclaim && kill(static_cast<pid_t>(expected), 0) != 0 && errno == ESRCH)) continue;
      if (!owner.compare_exchange_strong(expected, pid, std::memory_order_acq_rel)) continue;
      record_ = &records[index];
      return Ok();
    }
  }
  return ResourceExhausted("observation shared memory has no free process record");
}

bool ObservationPublisher::TryBecomePublisher() {
  if (!publisher_) publisher_ = LockByte(fd_, kPublisherByte, F_WRLCK, false);
  return publisher_;
}

}  // namespace telepath
