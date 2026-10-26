// Stress and boundary cases of the observation plane: readers in other
// processes never see a torn snapshot while frames churn, another process
// takes over publishing when the publisher is killed, process records run out
// and are reclaimed from dead processes, and foreign objects are refused.
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
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
constexpr std::size_t kPoolSize = 32;

auto Options(const std::string &name, std::chrono::milliseconds interval) -> telepath::BufferManagerOptions {
  telepath::BufferManagerOptions options;
  options.pool_size = kPoolSize;
  options.page_size = kPageSize;
  options.observation.enabled = true;
  options.observation.shared_memory_name = name;
  options.observation.publish_interval = interval;
  return options;
}

auto Open(const telepath::BufferManagerOptions &options, const std::string &root) -> telepath::Result<std::unique_ptr<telepath::BufferManager>> {
  return telepath::BufferManager::Create(options, std::make_unique<telepath::PosixDiskBackend>(root, telepath::BlockGeometry::Of<kPageSize>()), telepath::MakeClockSweepReplacer(options.pool_size), telepath::MakeCounterTelemetrySink());
}

auto Name(const char *test) -> std::string { return std::string("/telepath_") + test + "_" + std::to_string(getpid()); }

auto Spawn(const std::vector<std::string> &args) -> pid_t {
  std::vector<char *> argv;
  for (const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  assert(posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) == 0);
  return pid;
}

auto Wait(pid_t pid) -> int {
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// A read-only mapping read with the published seqlock protocol.
class Reader {
 public:
  explicit Reader(const std::string &name) {
    const int fd = shm_open(name.c_str(), O_RDONLY, 0);
    assert(fd >= 0);
    struct stat info{};
    assert(fstat(fd, &info) == 0);
    bytes_ = static_cast<std::size_t>(info.st_size);
    base_ = static_cast<const std::byte *>(mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, fd, 0));
    assert(base_ != MAP_FAILED);
    close(fd);
  }
  ~Reader() { munmap(const_cast<std::byte *>(base_), bytes_); }

  auto sequence() const -> std::uint64_t {
    return std::atomic_ref<const std::uint64_t>(reinterpret_cast<const obs::ObservationHeader *>(base_)->sequence).load(std::memory_order_acquire);
  }

  // Copies one consistent snapshot; false if a writer kept interfering for
  // seconds. The limit is time, not attempts: a publisher descheduled in the
  // middle of a snapshot keeps the sequence odd for as long as it waits.
  bool Snapshot(obs::ObservationHeader *header, std::vector<obs::ObservationFrameRecord> *frames) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const std::uint64_t before = sequence();
      if (before % 2 == 1) {
        std::this_thread::yield();
        continue;
      }
      std::memcpy(header, base_, sizeof(*header));
      frames->resize(header->pool_size);
      std::memcpy(frames->data(), base_ + header->frames_offset, frames->size() * sizeof(obs::ObservationFrameRecord));
      std::atomic_thread_fence(std::memory_order_acquire);
      if (sequence() == before) return true;
    }
    return false;
  }

  auto publisher() const -> std::uint32_t {
    obs::ObservationHeader header;
    std::vector<obs::ObservationFrameRecord> frames;
    assert(Snapshot(&header, &frames));
    return header.publisher_pid;
  }

 private:
  const std::byte *base_{nullptr};
  std::size_t bytes_{0};
};

// Child of ExpectNoTornSnapshots: reads snapshots for a while and checks that
// each summary matches its frames.
auto TornReadChild(const std::string &name, int milliseconds) -> int {
  Reader reader(name);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  std::uint64_t distinct = 0;
  std::uint64_t last = 0;
  obs::ObservationHeader header;
  std::vector<obs::ObservationFrameRecord> frames;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!reader.Snapshot(&header, &frames)) return 2;
    // Nothing is published before the first snapshot.
    if (header.published_unix_ms == 0) continue;
    obs::ObservationSummary counted{};
    for (std::size_t index = 0; index < frames.size(); ++index) {
      const auto &frame = frames[index];
      if (frame.frame_id != index) return 3;
      if (frame.state == obs::kFrameFree) ++counted.free_frames;
      else if (frame.state == obs::kFrameLoading) ++counted.loading_frames;
      else if (frame.state == obs::kFrameResident) ++counted.resident_frames;
      else return 4;
      if ((frame.flags & obs::kFrameDirty) != 0) ++counted.dirty_frames;
      if (frame.pins != 0) ++counted.pinned_frames;
      if ((frame.flags & obs::kFrameWritebackQueued) != 0) ++counted.writeback_queued_frames;
      if ((frame.flags & obs::kFrameWritebackActive) != 0) ++counted.writeback_active_frames;
    }
    counted.dirty_page_count = header.summary.dirty_page_count;
    if (std::memcmp(&counted, &header.summary, sizeof(counted)) != 0) {
      std::fprintf(stderr, "torn snapshot %llu: resident %llu/%llu dirty %llu/%llu pinned %llu/%llu\n", static_cast<unsigned long long>(header.sequence),
                   static_cast<unsigned long long>(header.summary.resident_frames), static_cast<unsigned long long>(counted.resident_frames),
                   static_cast<unsigned long long>(header.summary.dirty_frames), static_cast<unsigned long long>(counted.dirty_frames),
                   static_cast<unsigned long long>(header.summary.pinned_frames), static_cast<unsigned long long>(counted.pinned_frames));
      return 5;
    }
    if (header.sequence != last) ++distinct;
    last = header.sequence;
  }
  return distinct > 10 ? 0 : 6;
}

// Frames change state constantly while a C++ reader and the Python reader in
// other processes check every snapshot they see.
void ExpectNoTornSnapshots() {
  const std::string name = Name("obs_torn");
  telepath::buffer_test_support::TestRootGuard root("telepath_obs_torn");
  auto manager = Open(Options(name, std::chrono::milliseconds(1)), root.path().string());
  assert(manager.has_value());

  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      std::mt19937 rng(worker);
      while (!stop.load()) {
        const telepath::BlockId block = rng() % (kPoolSize * 3);
        if (rng() % 3 == 0) {
          auto handle = manager.value()->ReadBufferExclusive(1, block);
          if (handle.has_value()) handle->mutable_data()[0] = std::byte{1};
        } else {
          auto handle = manager.value()->ReadBuffer(1, block);
        }
        if (rng() % 50 == 0) std::ignore = manager.value()->FlushAll();
      }
    });
  }
  const pid_t cpp_reader = Spawn({"/proc/self/exe", "torn-read-child", name, "2000"});
  pid_t python_reader = -1;
#ifdef TELEPATH_PYTHON3
  python_reader = Spawn({TELEPATH_PYTHON3, TELEPATH_SOURCE_DIR "/test/python/observation_reader_test.py", "--consistency", name, "2"});
#endif
  const int cpp_status = Wait(cpp_reader);
  if (cpp_status != 0) std::fprintf(stderr, "C++ reader exited with %d\n", cpp_status);
  assert(cpp_status == 0);
  if (python_reader > 0) assert(Wait(python_reader) == 0);
  stop.store(true);
  for (auto &worker : workers) worker.join();
}

// Child of ExpectPublisherFailover: attaches to the shared cache, publishes
// first, and then waits to be killed.
auto PublisherChild(const std::string &segment, const std::string &name, const std::string &root) -> int {
  auto options = Options(name, std::chrono::milliseconds(5));
  options.shared_memory.name = segment;
  options.shared_memory.mode = telepath::SharedMemoryMode::kAttach;
  auto manager = Open(options, root);
  if (!manager.has_value()) return 2;
  if (!manager.value()->PublishObservation().has_value()) return 3;
  while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// The publisher dies without cleaning up; the kernel releases its lock and
// the next process to publish takes over.
void ExpectPublisherFailover() {
  const std::string segment = Name("obs_failover_segment");
  const std::string name = Name("obs_failover");
  telepath::buffer_test_support::TestRootGuard root("telepath_obs_failover");
  auto options = Options(name, std::chrono::hours(1));
  options.shared_memory.name = segment;
  options.shared_memory.mode = telepath::SharedMemoryMode::kCreate;
  auto manager = Open(options, root.path().string());
  assert(manager.has_value());

  const pid_t child = Spawn({"/proc/self/exe", "publisher-child", segment, name, root.path().string()});
  Reader reader(name);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (reader.publisher() != static_cast<std::uint32_t>(child)) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(manager.value()->PublishObservation().has_value());
  assert(reader.publisher() == static_cast<std::uint32_t>(child));

  assert(kill(child, SIGKILL) == 0);
  assert(Wait(child) == 128 + SIGKILL);
  const auto killed = std::chrono::steady_clock::now();
  while (true) {
    assert(manager.value()->PublishObservation().has_value());
    if (reader.publisher() == static_cast<std::uint32_t>(getpid())) break;
    // The kernel drops a dead process's locks shortly after it exits, not
    // necessarily before waitpid() returns.
    assert(std::chrono::steady_clock::now() - killed < std::chrono::seconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  manager->reset();
  // The killed child never detached, so the segment name outlives it.
  assert(telepath::BufferManager::RemoveSharedMemory(segment).has_value());
}

// Child of ExpectProcessRecordsRunOut: takes a process record and waits to be
// killed.
auto RecordChild(const std::string &name, const std::string &root, const std::string &ready) -> int {
  auto manager = Open(Options(name, std::chrono::hours(1)), root);
  if (!manager.has_value()) return 2;
  const int fd = open(ready.c_str(), O_CREAT | O_WRONLY, 0600);
  close(fd);
  while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// The area has 64 process records. A record of a process that died without
// cleaning up is reused once the others are taken.
void ExpectProcessRecordsRunOut() {
  const std::string name = Name("obs_records");
  telepath::buffer_test_support::TestRootGuard root("telepath_obs_records");
  const std::string ready = (root.path() / "ready").string();
  const pid_t child = Spawn({"/proc/self/exe", "record-child", name, root.path().string(), ready});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (access(ready.c_str(), F_OK) != 0) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::vector<std::unique_ptr<telepath::BufferManager>> managers;
  for (int index = 0; index < 63; ++index) {
    auto manager = Open(Options(name, std::chrono::hours(1)), root.path().string());
    assert(manager.has_value());
    managers.push_back(std::move(manager.value()));
  }
  auto full = Open(Options(name, std::chrono::hours(1)), root.path().string());
  assert(!full.has_value() && full.error().code() == telepath::ErrorCode::kResourceExhausted);

  assert(kill(child, SIGKILL) == 0);
  assert(Wait(child) == 128 + SIGKILL);
  auto reclaimed = Open(Options(name, std::chrono::hours(1)), root.path().string());
  assert(reclaimed.has_value());
  full = Open(Options(name, std::chrono::hours(1)), root.path().string());
  assert(!full.has_value() && full.error().code() == telepath::ErrorCode::kResourceExhausted);
  managers.clear();
  reclaimed->reset();
  // The last user removes the name only once the kernel has released the
  // killed child's lock, which may still be pending (concurrency.md, Known
  // Limits).
  shm_unlink(name.c_str());
}

// An object of the right size that is not an observation area is refused.
void ExpectForeignObjectIsRejected() {
  const std::string name = Name("obs_foreign");
  telepath::buffer_test_support::TestRootGuard root("telepath_obs_foreign");
  const std::size_t bytes = sizeof(obs::ObservationHeader) + obs::kObservationProcessCapacity * sizeof(obs::ObservationProcessRecord) + kPoolSize * sizeof(obs::ObservationFrameRecord);
  const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  assert(fd >= 0);
  assert(ftruncate(fd, static_cast<off_t>(bytes)) == 0);
  const std::uint32_t garbage = 0xDEADBEEF;
  assert(pwrite(fd, &garbage, sizeof(garbage), 0) == sizeof(garbage));
  close(fd);

  auto refused = Open(Options(name, std::chrono::hours(1)), root.path().string());
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kInvalidArgument);
  shm_unlink(name.c_str());
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 4 && std::strcmp(argv[1], "torn-read-child") == 0) return TornReadChild(argv[2], std::stoi(argv[3]));
  if (argc == 5 && std::strcmp(argv[1], "publisher-child") == 0) return PublisherChild(argv[2], argv[3], argv[4]);
  if (argc == 5 && std::strcmp(argv[1], "record-child") == 0) return RecordChild(argv[2], argv[3], argv[4]);

  ExpectNoTornSnapshots();
  ExpectPublisherFailover();
  ExpectProcessRecordsRunOut();
  ExpectForeignObjectIsRejected();
  return 0;
}
