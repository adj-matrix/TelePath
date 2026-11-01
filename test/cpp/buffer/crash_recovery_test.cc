// Cross-process mode when an attached process dies without detaching: every
// other process notices, stops using the segment (kFailed), and a manager
// created afterwards under the same name gets a fresh segment.
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <tuple>
#include <span>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "disk_io_helpers.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

extern char **environ;

namespace {

constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPoolSize = 16;
constexpr telepath::FileId kFile = 1;

auto Open(const std::string &segment, telepath::SharedMemoryMode mode, const std::string &root, telepath::CrashRecovery recovery = telepath::CrashRecovery::kSalvage) -> telepath::Result<std::unique_ptr<telepath::BufferManager>> {
  telepath::BufferManagerOptions options;
  options.pool_size = kPoolSize;
  options.page_size = kPageSize;
  options.shared_memory.name = segment;
  options.shared_memory.mode = mode;
  options.shared_memory.crash_recovery = recovery;
  return telepath::BufferManager::Create(options, std::make_unique<telepath::PosixDiskBackend>(root, telepath::BlockGeometry::Of<kPageSize>()), telepath::MakeClockSweepReplacer(kPoolSize), telepath::MakeNoOpTelemetrySink());
}

// Forwards to a PosixDiskBackend but never completes a read or write of one
// block, freezing the thread that waits for it; creates `ready` when that
// request arrives.
class StuckBackend : public telepath::DiskBackend {
 public:
  StuckBackend(const std::string &root, telepath::DiskOperation operation, telepath::BlockId block, std::string ready)
    : inner_(root, telepath::BlockGeometry::Of<kPageSize>()), operation_(operation), block_(block), ready_(std::move(ready)) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    if (requests.front().operation == operation_ && requests.front().tag.file_id == kFile && requests.front().tag.block_id == block_) {
      close(open(ready_.c_str(), O_CREAT | O_WRONLY, 0600));
      return std::size_t{1};
    }
    return inner_.Submit(requests.first(1), mode);
  }
  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override { return inner_.Reap(out, wait); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

 private:
  telepath::PosixDiskBackend inner_;
  telepath::DiskOperation operation_;
  telepath::BlockId block_;
  std::string ready_;
};

auto Name(const char *test) -> std::string { return std::string("/telepath_") + test + "_" + std::to_string(getpid()); }

auto Exists(const std::string &name) -> bool {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd >= 0) close(fd);
  return fd >= 0;
}

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

void WaitForFile(const std::string &path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (access(path.c_str(), F_OK) != 0) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// Waits until `manager` reports kFailed, as it must once it notices a dead
// process.
bool BecomesFailed(telepath::BufferManager *manager, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto handle = manager->ReadBuffer(kFile, 0);
    if (!handle.has_value()) return handle.error().code() == telepath::ErrorCode::kFailed && manager->failure().has_value();
    handle->Reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

// Child that attaches, uses a page, signals `ready`, and then either waits to
// be killed or, with `exit`, detaches cleanly.
auto AttachedChild(const std::string &segment, const std::string &root, const std::string &ready, bool exit) -> int {
  auto manager = Open(segment, telepath::SharedMemoryMode::kAttach, root);
  if (!manager.has_value()) return 2;
  {
    auto handle = manager.value()->ReadBufferExclusive(kFile, 1);
    if (!handle.has_value()) return 3;
    handle->mutable_data()[0] = std::byte{0x11};
  }
  close(open(ready.c_str(), O_CREAT | O_WRONLY, 0600));
  if (exit) return 0;
  while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}

auto PersistedByte(const std::filesystem::path &root, telepath::BlockId block) -> std::byte {
  telepath::PosixDiskBackend verifier(root.string(), telepath::BlockGeometry::Of<kPageSize>());
  std::array<std::byte, kPageSize> page{};
  assert(telepath::test_support::ReadBlock(verifier, {kFile, block}, page).has_value());
  return page[0];
}

// Child of ExpectCrashRecovery: leaves page 2 dirty, then holds page 3
// exclusively in the middle of a change until it is killed.
auto DirtyingChild(const std::string &segment, const std::string &root, const std::string &ready, const std::string &recovery) -> int {
  auto manager = Open(segment, telepath::SharedMemoryMode::kAttach, root, recovery == "discard" ? telepath::CrashRecovery::kDiscard : telepath::CrashRecovery::kSalvage);
  if (!manager.has_value()) return 2;
  {
    auto released = manager.value()->ReadBufferExclusive(kFile, 2);
    if (!released.has_value()) return 3;
    released->mutable_data()[0] = std::byte{0x22};
  }
  auto torn = manager.value()->ReadBufferExclusive(kFile, 3);
  if (!torn.has_value()) return 4;
  torn->mutable_data()[0] = std::byte{0x33};
  close(open(ready.c_str(), O_CREAT | O_WRONLY, 0600));
  while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// With kSalvage the process that notices the crash writes back every dirty
// page except those the dead process held exclusively, whose contents may be
// half changed; with kDiscard it writes back nothing.
void ExpectCrashRecovery(telepath::CrashRecovery recovery) {
  const bool salvage = recovery == telepath::CrashRecovery::kSalvage;
  const std::string segment = Name(salvage ? "crash_salvage" : "crash_discard");
  telepath::buffer_test_support::TestRootGuard root(salvage ? "telepath_crash_salvage" : "telepath_crash_discard");
  const std::string ready = (root.path() / "ready").string();
  auto survivor = Open(segment, telepath::SharedMemoryMode::kCreate, root.path().string(), recovery);
  assert(survivor.has_value());
  {
    auto own = survivor.value()->ReadBufferExclusive(kFile, 4);
    assert(own.has_value());
    own->mutable_data()[0] = std::byte{0x44};
  }
  const pid_t child = Spawn({"dirtying-child", segment, root.path().string(), ready, salvage ? "salvage" : "discard"});
  WaitForFile(ready);
  assert(kill(child, SIGKILL) == 0);
  assert(Wait(child) == 128 + SIGKILL);
  assert(BecomesFailed(survivor->get(), std::chrono::seconds(5)));
  // Destroying the manager waits for the recovery to finish.
  survivor->reset();

  assert(PersistedByte(root.path(), 2) == (salvage ? std::byte{0x22} : std::byte{0}));
  assert(PersistedByte(root.path(), 4) == (salvage ? std::byte{0x44} : std::byte{0}));
  assert(PersistedByte(root.path(), 3) == std::byte{0});
}

// Attaching with another crash-recovery policy is refused: every process must
// handle a crash the same way.
void ExpectRecoveryPolicyMustMatch() {
  const std::string segment = Name("crash_policy");
  telepath::buffer_test_support::TestRootGuard root("telepath_crash_policy");
  auto first = Open(segment, telepath::SharedMemoryMode::kCreate, root.path().string(), telepath::CrashRecovery::kSalvage);
  assert(first.has_value());
  auto other = Open(segment, telepath::SharedMemoryMode::kAttach, root.path().string(), telepath::CrashRecovery::kDiscard);
  assert(!other.has_value() && other.error().code() == telepath::ErrorCode::kInvalidArgument);
}

// Child of ExpectSurvivorsDoNotHang: dies holding a page exclusively, in the
// middle of reading a page, or in the middle of writing one back.
auto StuckChild(const std::string &segment, const std::string &root, const std::string &ready, const std::string &mode) -> int {
  telepath::BufferManagerOptions options;
  options.pool_size = kPoolSize;
  options.page_size = kPageSize;
  options.shared_memory.name = segment;
  options.shared_memory.mode = telepath::SharedMemoryMode::kAttach;
  const telepath::DiskOperation operation = mode == "writeback" ? telepath::DiskOperation::kWrite : telepath::DiskOperation::kRead;
  auto manager = telepath::BufferManager::Create(options, std::make_unique<StuckBackend>(root, operation, mode == "exclusive" ? 1000 : 5, ready), telepath::MakeClockSweepReplacer(kPoolSize), telepath::MakeNoOpTelemetrySink());
  if (!manager.has_value()) return 2;
  if (mode == "exclusive") {
    auto held = manager.value()->ReadBufferExclusive(kFile, 5);
    if (!held.has_value()) return 3;
    held->mutable_data()[0] = std::byte{1};
    close(open(ready.c_str(), O_CREAT | O_WRONLY, 0600));
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (mode == "writeback") {
    auto dirty = manager.value()->ReadBufferExclusive(kFile, 5);
    if (!dirty.has_value()) return 3;
    dirty->mutable_data()[0] = std::byte{1};
    dirty->Reset();
  }
  // Blocks forever: the backend never completes this read or write.
  if (mode == "load") std::ignore = manager.value()->ReadBuffer(kFile, 5);
  else std::ignore = manager.value()->FlushBuffer(kFile, 5);
  return 4;
}

// Runs `call` with a deadline; a call that never returns fails the test.
template <typename Call>
auto WithinDeadline(const char *what, Call call) {
  auto result = std::async(std::launch::async, call);
  if (result.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    std::fprintf(stderr, "%s did not return after a crash\n", what);
    std::abort();
  }
  return result.get();
}

bool IsFailed(const telepath::Status &status) { return !status.has_value() && status.error().code() == telepath::ErrorCode::kFailed; }

template <typename Value>
auto StatusOf(const telepath::Result<Value> &result) -> telepath::Status {
  if (result.has_value()) return telepath::Ok();
  return std::unexpected(result.error());
}

// A survivor waiting for something only the dead process could finish (its
// exclusive latch, its read, its writeback) returns kFailed instead of
// waiting forever, and so do Close() and destruction.
void ExpectSurvivorsDoNotHang(const char *mode) {
  const std::string segment = Name((std::string("crash_stuck_") + mode).c_str());
  telepath::buffer_test_support::TestRootGuard root((std::string("telepath_crash_stuck_") + mode).c_str());
  const std::string ready = (root.path() / "ready").string();
  auto survivor = Open(segment, telepath::SharedMemoryMode::kCreate, root.path().string());
  assert(survivor.has_value());
  telepath::BufferManager *manager = survivor->get();
  const pid_t child = Spawn({"stuck-child", segment, root.path().string(), ready, mode});
  WaitForFile(ready);
  // Let the survivor start waiting before the child dies.
  auto read = std::async(std::launch::async, [&] { return manager->ReadBuffer(kFile, 5); });
  auto flush = std::async(std::launch::async, [&] { return manager->FlushBuffer(kFile, 5); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  assert(kill(child, SIGKILL) == 0);
  assert(Wait(child) == 128 + SIGKILL);

  if (read.wait_for(std::chrono::seconds(10)) != std::future_status::ready || flush.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    std::fprintf(stderr, "%s: a waiting survivor did not return after the crash\n", mode);
    std::abort();
  }
  // During a stuck writeback the page stays resident and unlatched, so a read
  // may still succeed before the fence; it only has to return.
  const telepath::Status read_result = StatusOf(read.get());
  assert(IsFailed(read_result) || (std::strcmp(mode, "writeback") == 0 && read_result.has_value()));
  telepath::Status flush_result = flush.get();
  assert(flush_result.has_value() || IsFailed(flush_result));
  assert(IsFailed(WithinDeadline("ReadBufferExclusive", [&] { return StatusOf(manager->ReadBufferExclusive(kFile, 5)); })));
  assert(IsFailed(WithinDeadline("FlushAll", [&] { return manager->FlushAll(); })));
  assert(IsFailed(WithinDeadline("Close", [&] { return manager->Close(); })));
  WithinDeadline("destruction", [&] {
    survivor->reset();
    return 0;
  });
}

void ExpectCrashFencesSurvivors() {
  const std::string segment = Name("crash_fence");
  telepath::buffer_test_support::TestRootGuard root("telepath_crash_fence");
  const std::string ready = (root.path() / "ready").string();
  auto survivor = Open(segment, telepath::SharedMemoryMode::kCreate, root.path().string());
  assert(survivor.has_value());
  const pid_t child = Spawn({"attached-child", segment, root.path().string(), ready, "stay"});
  WaitForFile(ready);
  assert(kill(child, SIGKILL) == 0);
  assert(Wait(child) == 128 + SIGKILL);
  assert(BecomesFailed(survivor->get(), std::chrono::seconds(5)));
  // The crashed segment's name is gone, so nobody joins it any more.
  assert(!Exists(segment));
}

void ExpectCleanExitDoesNotFence() {
  const std::string segment = Name("crash_clean_exit");
  telepath::buffer_test_support::TestRootGuard root("telepath_crash_clean_exit");
  const std::string ready = (root.path() / "ready").string();
  auto survivor = Open(segment, telepath::SharedMemoryMode::kCreate, root.path().string());
  assert(survivor.has_value());
  assert(Wait(Spawn({"attached-child", segment, root.path().string(), ready, "exit"})) == 0);
  assert(!BecomesFailed(survivor->get(), std::chrono::milliseconds(300)));
  auto handle = survivor.value()->ReadBuffer(kFile, 1);
  assert(handle.has_value() && handle->data()[0] == std::byte{0x11});
}

// After a crash, a manager created under the same name gets a fresh segment,
// and a fenced survivor leaving later does not remove that new segment.
void ExpectRebuildAfterCrash() {
  const std::string segment = Name("crash_rebuild");
  telepath::buffer_test_support::TestRootGuard root("telepath_crash_rebuild");
  const std::string ready = (root.path() / "ready").string();
  auto survivor = Open(segment, telepath::SharedMemoryMode::kCreate, root.path().string());
  assert(survivor.has_value());
  const pid_t child = Spawn({"attached-child", segment, root.path().string(), ready, "stay"});
  WaitForFile(ready);
  assert(kill(child, SIGKILL) == 0);
  assert(Wait(child) == 128 + SIGKILL);
  assert(BecomesFailed(survivor->get(), std::chrono::seconds(5)));

  auto rebuilt = Open(segment, telepath::SharedMemoryMode::kCreateOrAttach, root.path().string());
  assert(rebuilt.has_value());
  assert(rebuilt.value()->ExportSnapshot().dirty_page_count == 0);
  survivor->reset();
  assert(Exists(segment));
  auto joined = Open(segment, telepath::SharedMemoryMode::kAttach, root.path().string());
  assert(joined.has_value());
  assert(joined.value()->ReadBuffer(kFile, 0).has_value());
  joined->reset();
  rebuilt->reset();
  assert(!Exists(segment));
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 6 && std::strcmp(argv[1], "attached-child") == 0) return AttachedChild(argv[2], argv[3], argv[4], std::strcmp(argv[5], "exit") == 0);
  if (argc == 6 && std::strcmp(argv[1], "dirtying-child") == 0) return DirtyingChild(argv[2], argv[3], argv[4], argv[5]);
  if (argc == 6 && std::strcmp(argv[1], "stuck-child") == 0) return StuckChild(argv[2], argv[3], argv[4], argv[5]);

  ExpectCrashFencesSurvivors();
  ExpectCleanExitDoesNotFence();
  ExpectRebuildAfterCrash();
  ExpectCrashRecovery(telepath::CrashRecovery::kSalvage);
  ExpectCrashRecovery(telepath::CrashRecovery::kDiscard);
  ExpectRecoveryPolicyMustMatch();
  ExpectSurvivorsDoNotHang("exclusive");
  ExpectSurvivorsDoNotHang("load");
  ExpectSurvivorsDoNotHang("writeback");
  return 0;
}
