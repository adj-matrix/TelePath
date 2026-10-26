// Boundary and stress cases of cross-process mode: process slot exhaustion,
// foreign objects under the segment name, many processes racing to create
// one segment, attach racing with removal, and durability of writes by a
// process that exits before syncing.
#include <fcntl.h>
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
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

extern char **environ;

namespace {

constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPoolSize = 8;
constexpr int kRacingProcesses = 8;
// More files than the segment's table of files awaiting a sync holds.
constexpr telepath::FileId kManyFiles = 1100;

// Forwards to a PosixDiskBackend and records which files it synced.
class SyncRecordingBackend : public telepath::DiskBackend {
 public:
  explicit SyncRecordingBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kPageSize>()) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      for (const auto &request : requests) {
        if (request.operation == telepath::DiskOperation::kSync) synced_.insert(request.tag.file_id);
      }
    }
    return inner_.Submit(requests, mode);
  }
  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override { return inner_.Reap(out, wait); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

  auto synced() -> std::set<telepath::FileId> {
    std::lock_guard<std::mutex> guard(latch_);
    return synced_;
  }

 private:
  telepath::PosixDiskBackend inner_;
  std::mutex latch_;
  std::set<telepath::FileId> synced_;
};

auto Options(const std::string &name, telepath::SharedMemoryMode mode) -> telepath::BufferManagerOptions {
  telepath::BufferManagerOptions options;
  options.pool_size = kPoolSize;
  options.page_size = kPageSize;
  options.shared_memory.name = name;
  options.shared_memory.mode = mode;
  return options;
}

auto Open(const telepath::BufferManagerOptions &options, const std::string &root, SyncRecordingBackend **backend = nullptr) -> telepath::Result<std::unique_ptr<telepath::BufferManager>> {
  auto owned = std::make_unique<SyncRecordingBackend>(root);
  if (backend != nullptr) *backend = owned.get();
  return telepath::BufferManager::Create(options, std::move(owned), telepath::MakeClockSweepReplacer(options.pool_size), telepath::MakeNoOpTelemetrySink());
}

auto Exists(const std::string &name) -> bool {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd >= 0) close(fd);
  return fd >= 0;
}

auto Name(const char *test) -> std::string { return std::string("/telepath_") + test + "_" + std::to_string(getpid()); }

auto Spawn(const std::vector<std::string> &args) -> pid_t {
  std::vector<char *> argv{const_cast<char *>("/proc/self/exe")};
  for (const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  assert(posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv.data(), environ) == 0);
  return pid;
}

auto Wait(pid_t pid) -> int {
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// Waits for a child and reports how it ended if it failed, so a rare failure
// names the step (each child returns a distinct code; 66 is a sanitizer
// report).
void ExpectChildSucceeded(pid_t pid, const char *what) {
  const int status = Wait(pid);
  if (status == 0) return;
  std::fprintf(stderr, "%s: child %d ended with %d\n", what, static_cast<int>(pid), status);
  std::abort();
}

void WriteMarker(telepath::BufferManager *manager, telepath::FileId file, telepath::BlockId block, std::byte value) {
  auto handle = manager->ReadBufferExclusive(file, block);
  assert(handle.has_value());
  handle->mutable_data()[0] = value;
}

// A segment has 64 process slots; every attached manager takes one.
void ExpectProcessSlotsRunOut() {
  const std::string name = Name("slots");
  telepath::buffer_test_support::TestRootGuard root("telepath_shm_slots");
  std::vector<std::unique_ptr<telepath::BufferManager>> managers;
  for (int index = 0; index < 64; ++index) {
    auto manager = Open(Options(name, telepath::SharedMemoryMode::kCreateOrAttach), root.path().string());
    assert(manager.has_value());
    managers.push_back(std::move(manager.value()));
  }
  auto refused = Open(Options(name, telepath::SharedMemoryMode::kAttach), root.path().string());
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kResourceExhausted);
  managers.pop_back();
  auto admitted = Open(Options(name, telepath::SharedMemoryMode::kAttach), root.path().string());
  assert(admitted.has_value());
  WriteMarker(admitted->get(), 1, 0, std::byte{0x42});
  auto check = managers.front()->ReadBuffer(1, 0);
  assert(check.has_value() && check->data()[0] == std::byte{0x42});
  check->Reset();
  admitted->reset();
  managers.clear();
  assert(!Exists(name));
}

// Something else under the segment name is refused, never trusted.
void ExpectForeignObjectsAreRejected() {
  telepath::buffer_test_support::TestRootGuard root("telepath_shm_foreign");
  const auto plant = [](const std::string &name, std::uint64_t magic, std::uint32_t version) {
    const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, 1 << 20) == 0);
    auto *base = static_cast<std::byte *>(mmap(nullptr, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(base != MAP_FAILED);
    const std::uint32_t ready = 1;
    std::memcpy(base, &magic, sizeof(magic));
    std::memcpy(base + 8, &version, sizeof(version));
    std::memcpy(base + 12, &ready, sizeof(ready));
    munmap(base, 1 << 20);
    close(fd);
  };
  const std::string wrong_magic = Name("foreign_magic");
  plant(wrong_magic, 0x1234, 1);
  auto refused = Open(Options(wrong_magic, telepath::SharedMemoryMode::kCreateOrAttach), root.path().string());
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kInvalidArgument);
  assert(Exists(wrong_magic));
  assert(telepath::BufferManager::RemoveSharedMemory(wrong_magic).has_value());

  const std::string wrong_version = Name("foreign_version");
  plant(wrong_version, 0x3147455354504c54ULL, 99);
  refused = Open(Options(wrong_version, telepath::SharedMemoryMode::kAttach), root.path().string());
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kInvalidArgument);
  assert(telepath::BufferManager::RemoveSharedMemory(wrong_version).has_value());
}

// Child of ExpectRacingCreatorsShareOneSegment: writes its own page, then
// waits until it sees every racer's page in the same segment.
auto RacingChild(const std::string &name, const std::string &root, int index) -> int {
  auto manager = Open(Options(name, telepath::SharedMemoryMode::kCreateOrAttach), root);
  if (!manager.has_value()) return 2;
  WriteMarker(manager->get(), 1, static_cast<telepath::BlockId>(index), static_cast<std::byte>(0x10 + index));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  for (int other = 0; other < kRacingProcesses;) {
    auto handle = manager.value()->ReadBuffer(1, static_cast<telepath::BlockId>(other));
    if (!handle.has_value()) return 3;
    if (handle->data()[0] == static_cast<std::byte>(0x10 + other)) {
      ++other;
      continue;
    }
    handle->Reset();
    if (std::chrono::steady_clock::now() > deadline) return 4;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 0;
}

// Processes that all start with kCreateOrAttach at once end up in one segment.
void ExpectRacingCreatorsShareOneSegment() {
  const std::string name = Name("racing");
  telepath::buffer_test_support::TestRootGuard root("telepath_shm_racing");
  std::vector<pid_t> children;
  for (int index = 0; index < kRacingProcesses; ++index) children.push_back(Spawn({"racing-child", name, root.path().string(), std::to_string(index)}));
  for (pid_t child : children) ExpectChildSucceeded(child, "racing creator");
  assert(!Exists(name));
}

// Managers that attach while the last one leaves either join it before the
// name is removed or create a fresh segment; none fails or leaks the name.
void ExpectAttachRacesWithRemoval() {
  const std::string name = Name("churn");
  telepath::buffer_test_support::TestRootGuard root("telepath_shm_churn");
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&, thread] {
      for (int round = 0; round < 100; ++round) {
        auto manager = Open(Options(name, telepath::SharedMemoryMode::kCreateOrAttach), root.path().string());
        if (!manager.has_value()) {
          std::fprintf(stderr, "churn: open failed with %d: %s\n", static_cast<int>(manager.error().code()), manager.error().message());
          failures.fetch_add(1);
          continue;
        }
        WriteMarker(manager->get(), 2, static_cast<telepath::BlockId>(thread), std::byte{1});
      }
    });
  }
  for (auto &thread : threads) thread.join();
  assert(failures.load() == 0);
  assert(!Exists(name));
}

// Child of ExpectWritesOfAnExitedProcessAreSynced: writes one page to each of
// many files, waits for the writes, records the files it synced itself, and
// exits without FlushAll() or Close().
auto WritingChild(const std::string &name, const std::string &root, const std::string &record) -> int {
  SyncRecordingBackend *backend = nullptr;
  auto manager = Open(Options(name, telepath::SharedMemoryMode::kAttach), root, &backend);
  if (!manager.has_value()) return 2;
  for (telepath::FileId file = 1; file <= kManyFiles; ++file) {
    WriteMarker(manager->get(), file, 0, std::byte{0x5A});
    if (!manager.value()->FlushBuffer(file, 0).has_value()) return 3;
  }
  std::ofstream out(record);
  for (telepath::FileId file : backend->synced()) out << file << "\n";
  return 0;
}

// FlushAll() in one process makes durable what another process wrote back,
// even after that process exited and even beyond the segment's table of files
// awaiting a sync.
void ExpectWritesOfAnExitedProcessAreSynced() {
  const std::string name = Name("sync_table");
  telepath::buffer_test_support::TestRootGuard root("telepath_shm_sync_table");
  SyncRecordingBackend *backend = nullptr;
  auto manager = Open(Options(name, telepath::SharedMemoryMode::kCreate), root.path().string(), &backend);
  assert(manager.has_value());
  const std::string record = (root.path() / "child_syncs.txt").string();
  ExpectChildSucceeded(Spawn({"writing-child", name, root.path().string(), record}), "writing child");
  assert(manager.value()->FlushAll().has_value());

  std::set<telepath::FileId> synced = backend->synced();
  std::ifstream in(record);
  for (telepath::FileId file = 0; in >> file;) synced.insert(file);
  for (telepath::FileId file = 1; file <= kManyFiles; ++file) {
    if (synced.contains(file)) continue;
    std::fprintf(stderr, "file %u was written back but never synced\n", file);
    std::abort();
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 5 && std::strcmp(argv[1], "racing-child") == 0) return RacingChild(argv[2], argv[3], std::stoi(argv[4]));
  if (argc == 5 && std::strcmp(argv[1], "writing-child") == 0) return WritingChild(argv[2], argv[3], argv[4]);

  ExpectProcessSlotsRunOut();
  ExpectForeignObjectsAreRejected();
  ExpectRacingCreatorsShareOneSegment();
  ExpectAttachRacesWithRemoval();
  ExpectWritesOfAnExitedProcessAreSynced();
  return 0;
}
