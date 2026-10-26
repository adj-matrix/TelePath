#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/observation/observation_format.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

extern char **environ;

namespace {

namespace obs = telepath::observation;

constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPoolSize = 8;

// Read-only view of an observation area, as another process would map it.
class Area {
 public:
  explicit Area(const std::string &name) {
    const int fd = shm_open(name.c_str(), O_RDONLY, 0);
    assert(fd >= 0);
    struct stat info{};
    assert(fstat(fd, &info) == 0);
    bytes_ = static_cast<std::size_t>(info.st_size);
    base_ = static_cast<const std::byte *>(mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, fd, 0));
    assert(base_ != MAP_FAILED);
    close(fd);
  }
  ~Area() { munmap(const_cast<std::byte *>(base_), bytes_); }

  auto header() const -> obs::ObservationHeader {
    obs::ObservationHeader header;
    std::memcpy(&header, base_, sizeof(header));
    return header;
  }
  auto sequence() const -> std::uint64_t {
    return std::atomic_ref<const std::uint64_t>(reinterpret_cast<const obs::ObservationHeader *>(base_)->sequence).load(std::memory_order_acquire);
  }
  auto frame(std::size_t index) const -> obs::ObservationFrameRecord {
    obs::ObservationFrameRecord record;
    std::memcpy(&record, base_ + header().frames_offset + index * sizeof(record), sizeof(record));
    return record;
  }
  auto processes() const -> std::vector<obs::ObservationProcessRecord> {
    std::vector<obs::ObservationProcessRecord> live;
    for (std::size_t index = 0; index < obs::kObservationProcessCapacity; ++index) {
      obs::ObservationProcessRecord record;
      std::memcpy(&record, base_ + header().processes_offset + index * sizeof(record), sizeof(record));
      if (record.pid != 0) live.push_back(record);
    }
    return live;
  }

 private:
  const std::byte *base_{nullptr};
  std::size_t bytes_{0};
};

auto Options(const std::string &observation_name, std::chrono::milliseconds interval = std::chrono::hours(1)) -> telepath::BufferManagerOptions {
  telepath::BufferManagerOptions options;
  options.pool_size = kPoolSize;
  options.page_size = kPageSize;
  options.observation.enabled = true;
  options.observation.shared_memory_name = observation_name;
  options.observation.publish_interval = interval;
  return options;
}

auto Open(const telepath::BufferManagerOptions &options, const std::string &root) -> telepath::Result<std::unique_ptr<telepath::BufferManager>> {
  return telepath::BufferManager::Create(options, std::make_unique<telepath::PosixDiskBackend>(root, telepath::BlockGeometry::Of<kPageSize>()), telepath::MakeClockSweepReplacer(options.pool_size), telepath::MakeCounterTelemetrySink());
}

auto Exists(const std::string &name) -> bool {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd >= 0) close(fd);
  return fd >= 0;
}

auto Spawn(const std::vector<std::string> &args) -> int {
  std::vector<char *> argv;
  for (const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return 127;
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

auto Name(const char *test) -> std::string { return std::string("/telepath_") + test + "_" + std::to_string(getpid()); }

void ExpectPublishedSnapshot() {
  const std::string name = Name("observation");
  telepath::buffer_test_support::TestRootGuard root("telepath_observation");
  auto manager = Open(Options(name), root.path().string());
  assert(manager.has_value());
  {
    auto page = manager.value()->ReadBufferExclusive(3, 5);
    assert(page.has_value());
    page->mutable_data()[0] = std::byte{1};
    page->MarkDirty(7);
    auto pinned = manager.value()->ReadBuffer(3, 6);
    assert(pinned.has_value());
    assert(manager.value()->ReadBuffer(3, 6).has_value());
    assert(manager.value()->PublishObservation().has_value());
  }

  Area area(name);
  const obs::ObservationHeader header = area.header();
  assert(header.magic == obs::kObservationMagic && header.version == obs::kObservationVersion);
  assert(header.pool_size == kPoolSize && header.page_size == kPageSize && header.cross_process == 0);
  assert(header.sequence == 2 && header.publisher_pid == static_cast<std::uint32_t>(getpid()) && header.published_unix_ms != 0);
  assert(header.summary.resident_frames == 2 && header.summary.free_frames == kPoolSize - 2);
  assert(header.summary.dirty_frames == 1 && header.summary.dirty_page_count == 1);
  assert(header.summary.pinned_frames == 2);
  assert(header.min_rec_lsn == 7);

  std::size_t matched = 0;
  for (std::size_t index = 0; index < kPoolSize; ++index) {
    const obs::ObservationFrameRecord frame = area.frame(index);
    assert(frame.frame_id == index && frame.namespace_id == 0);
    if (frame.state != obs::kFrameResident) continue;
    assert(frame.file_id == 3);
    if (frame.block_id == 5) assert((frame.flags & obs::kFrameDirty) != 0 && frame.pins == 1);
    if (frame.block_id == 6) assert(frame.flags == 0 && frame.pins == 1);
    ++matched;
  }
  assert(matched == 2);

  const auto processes = area.processes();
  assert(processes.size() == 1 && processes[0].pid == static_cast<std::uint32_t>(getpid()));
  assert(processes[0].buffer_misses == 2 && processes[0].buffer_hits == 1);

#ifdef TELEPATH_PYTHON3
  const std::string expected = R"({"pool_size": 8, "page_size": 4096, "cross_process": false, "min_rec_lsn": 7, "summary": {"resident_frames": 2, "dirty_frames": 1, "pinned_frames": 0}, "counters": {"buffer_misses": 2, "buffer_hits": 1}})";
  assert(manager.value()->PublishObservation().has_value());
  assert(Spawn({TELEPATH_PYTHON3, TELEPATH_SOURCE_DIR "/test/python/observation_reader_test.py", "--live", name, expected}) == 0);
#endif

  manager->reset();
  assert(!Exists(name));
}

void ExpectBackgroundPublishing() {
  const std::string name = Name("observation_interval");
  telepath::buffer_test_support::TestRootGuard root("telepath_observation_interval");
  auto manager = Open(Options(name, std::chrono::milliseconds(5)), root.path().string());
  assert(manager.has_value());
  Area area(name);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (area.sequence() < 4) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(manager.value()->Close().has_value());
}

void ExpectSharingRules() {
  const std::string name = Name("observation_rules");
  telepath::buffer_test_support::TestRootGuard root("telepath_observation_rules");
  auto first = Open(Options(name), root.path().string());
  assert(first.has_value());

  auto other_pool = Options(name);
  other_pool.pool_size = kPoolSize * 2;
  auto mismatch = Open(other_pool, root.path().string());
  assert(!mismatch.has_value() && mismatch.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto bad_name = Open(Options("telepath_no_slash"), root.path().string());
  assert(!bad_name.has_value() && bad_name.error().code() == telepath::ErrorCode::kInvalidArgument);

  // A second user keeps the name alive after the first one goes away.
  auto second = Open(Options(name), root.path().string());
  assert(second.has_value());
  first->reset();
  assert(Exists(name));
  second->reset();
  assert(!Exists(name));
}

auto ChildAttach(const std::string &segment, const std::string &observation_name, const std::string &root, std::uint32_t parent) -> int {
  auto options = Options(observation_name);
  options.shared_memory.name = segment;
  options.shared_memory.mode = telepath::SharedMemoryMode::kAttach;
  auto manager = Open(options, root);
  if (!manager.has_value()) return 2;
  if (!manager.value()->ReadBuffer(9, 0).has_value()) return 3;
  if (!manager.value()->PublishObservation().has_value()) return 4;
  Area area(observation_name);
  const obs::ObservationHeader header = area.header();
  // The parent published first and keeps the frame snapshot to itself.
  if (header.cross_process != 1 || header.publisher_pid != parent) return 5;
  const auto processes = area.processes();
  if (processes.size() != 2) return 6;
  for (const auto &process : processes) {
    if (process.pid == static_cast<std::uint32_t>(getpid()) && process.buffer_hits != 1) return 7;
  }
  return 0;
}

void ExpectCrossProcessPublishing() {
  const std::string segment = Name("observation_segment");
  const std::string name = Name("observation_shared");
  telepath::buffer_test_support::TestRootGuard root("telepath_observation_shared");
  auto options = Options(name);
  options.shared_memory.name = segment;
  options.shared_memory.mode = telepath::SharedMemoryMode::kCreate;
  auto manager = Open(options, root.path().string());
  assert(manager.has_value());
  assert(manager.value()->ReadBuffer(9, 0).has_value());
  assert(manager.value()->PublishObservation().has_value());

  assert(Spawn({"/proc/self/exe", "child-attach", segment, name, root.path().string(), std::to_string(getpid())}) == 0);

  Area area(name);
  assert(area.processes().size() == 1);
  manager->reset();
  assert(!Exists(name));
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 6 && std::strcmp(argv[1], "child-attach") == 0) return ChildAttach(argv[2], argv[3], argv[4], static_cast<std::uint32_t>(std::stoul(argv[5])));

  ExpectPublishedSnapshot();
  ExpectBackgroundPublishing();
  ExpectSharingRules();
  ExpectCrossProcessPublishing();
  return 0;
}
