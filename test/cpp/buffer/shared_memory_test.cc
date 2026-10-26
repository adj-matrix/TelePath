#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
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
constexpr std::size_t kPoolSize = 16;
constexpr telepath::FileId kFile = 1;
constexpr telepath::BlockId kStressBlocks = 64;
constexpr int kStressRounds = 200;

// Forwards to a PosixDiskBackend and counts read requests.
class ReadCountingBackend : public telepath::DiskBackend {
 public:
  ReadCountingBackend(const std::string &root, std::atomic<int> *reads) : inner_(root, telepath::BlockGeometry::Of<kPageSize>()), reads_(reads) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    for (const auto &request : requests) {
      if (request.operation == telepath::DiskOperation::kRead) reads_->fetch_add(1);
    }
    return inner_.Submit(requests, mode);
  }
  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override { return inner_.Reap(out, wait); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

 private:
  telepath::PosixDiskBackend inner_;
  std::atomic<int> *reads_;
};

auto Options(const std::string &name, telepath::SharedMemoryMode mode, std::size_t pool_size = kPoolSize) -> telepath::BufferManagerOptions {
  telepath::BufferManagerOptions options;
  options.pool_size = pool_size;
  options.page_size = kPageSize;
  options.shared_memory.name = name;
  options.shared_memory.mode = mode;
  return options;
}

auto Open(const telepath::BufferManagerOptions &options, const std::string &root, std::atomic<int> *reads, std::unique_ptr<telepath::Replacer> replacer = nullptr)
  -> telepath::Result<std::unique_ptr<telepath::BufferManager>> {
  if (replacer == nullptr) replacer = telepath::MakeClockSweepReplacer(options.pool_size);
  return telepath::BufferManager::Create(options, std::make_unique<ReadCountingBackend>(root, reads), std::move(replacer), telepath::MakeNoOpTelemetrySink());
}

auto ReadByte(telepath::BufferManager *manager, telepath::BlockId block) -> std::byte {
  auto handle = manager->ReadBuffer(kFile, block);
  assert(handle.has_value());
  return handle->data()[0];
}

void WriteByte(telepath::BufferManager *manager, telepath::BlockId block, std::byte value) {
  auto handle = manager->ReadBufferExclusive(kFile, block);
  assert(handle.has_value());
  handle->mutable_data()[0] = value;
}

auto RunChild(const std::vector<std::string> &args) -> int {
  std::vector<char *> argv;
  std::string self = "/proc/self/exe";
  argv.push_back(self.data());
  for (const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  assert(posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv.data(), environ) == 0);
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

auto SegmentName(const char *test) -> std::string { return std::string("/telepath_") + test + "_" + std::to_string(getpid()); }

// Child: sees the parent's unwritten page without reading it from disk, then
// dirties a page of its own and leaves it for the parent.
auto ChildSeesParentPage(const std::string &name, const std::string &root) -> int {
  std::atomic<int> reads{0};
  auto manager = Open(Options(name, telepath::SharedMemoryMode::kAttach), root, &reads);
  if (!manager.has_value()) return 2;
  if (ReadByte(manager->get(), 0) != std::byte{0xA1}) return 3;
  if (reads.load() != 0) return 4;
  WriteByte(manager->get(), 1, std::byte{0xB2});
  if (manager.value()->ExportSnapshot().dirty_page_count != 2) return 5;
  {
    auto logged = manager.value()->ReadBufferExclusive(kFile, 1);
    if (!logged.has_value()) return 6;
    logged->MarkDirty(5);
  }
  if (manager.value()->MinRecLsn() != telepath::Lsn{5}) return 7;
  return 0;
}

void ExpectPagesAreSharedBetweenProcesses() {
  const std::string name = SegmentName("shared_pages");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_pages");
  std::atomic<int> reads{0};
  auto manager = Open(Options(name, telepath::SharedMemoryMode::kCreate), root.path().string(), &reads);
  assert(manager.has_value());
  WriteByte(manager->get(), 0, std::byte{0xA1});
  const int before = reads.load();

  assert(RunChild({"child-sees-parent-page", name, root.path().string()}) == 0);

  assert(ReadByte(manager->get(), 1) == std::byte{0xB2});
  assert(reads.load() == before);
  assert(manager.value()->ExportSnapshot().dirty_page_count == 2);
  // The recLSN the child reported lives in the shared descriptors.
  assert(manager.value()->MinRecLsn() == telepath::Lsn{5});

  // Close writes back the page the child dirtied through this process's backend.
  assert(manager.value()->Close().has_value());
  manager->reset();
  std::atomic<int> verify_reads{0};
  auto fresh = Open(Options(name, telepath::SharedMemoryMode::kCreate), root.path().string(), &verify_reads);
  assert(fresh.has_value());
  assert(ReadByte(fresh->get(), 0) == std::byte{0xA1});
  assert(ReadByte(fresh->get(), 1) == std::byte{0xB2});
  assert(verify_reads.load() == 2);
}

// Each process repeatedly rewrites its own half of a working set four times
// larger than the pool, so both evict, write back, and reload each other's
// pages through the shared page table and replacer.
auto Stress(telepath::BufferManager *manager, int parity) -> bool {
  std::atomic<bool> ok{true};
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&, thread] {
      for (int round = 1; round <= kStressRounds; ++round) {
        for (telepath::BlockId block = static_cast<telepath::BlockId>(parity + 2 * thread); block < kStressBlocks; block += 8) {
          const auto value = static_cast<std::byte>(round);
          {
            auto handle = manager->ReadBufferExclusive(kFile, block);
            if (!handle.has_value()) {
              ok = false;
              return;
            }
            if (round > 1 && handle->data()[0] != static_cast<std::byte>(round - 1)) ok = false;
            handle->mutable_data()[0] = value;
          }
        }
      }
    });
  }
  for (auto &thread : threads) thread.join();
  return ok.load();
}

auto ChildStress(const std::string &name, const std::string &root) -> int {
  std::atomic<int> reads{0};
  auto manager = Open(Options(name, telepath::SharedMemoryMode::kAttach), root, &reads);
  if (!manager.has_value()) return 2;
  return Stress(manager->get(), 1) ? 0 : 3;
}

void ExpectConcurrentProcessesKeepPagesIntact() {
  const std::string name = SegmentName("shared_stress");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_stress");
  std::atomic<int> reads{0};
  auto manager = Open(Options(name, telepath::SharedMemoryMode::kCreate), root.path().string(), &reads);
  assert(manager.has_value());

  int child_status = -1;
  std::thread child([&] { child_status = RunChild({"child-stress", name, root.path().string()}); });
  assert(Stress(manager->get(), 0));
  child.join();
  assert(child_status == 0);

  for (telepath::BlockId block = 0; block < kStressBlocks; ++block) assert(ReadByte(manager->get(), block) == static_cast<std::byte>(kStressRounds));
  assert(manager.value()->Close().has_value());
}

void ExpectModesAndValidation() {
  const std::string name = SegmentName("shared_modes");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_modes");
  std::atomic<int> reads{0};

  auto missing = Open(Options(name, telepath::SharedMemoryMode::kAttach), root.path().string(), &reads);
  assert(!missing.has_value() && missing.error().code() == telepath::ErrorCode::kNotFound);

  auto first = Open(Options(name, telepath::SharedMemoryMode::kCreateOrAttach), root.path().string(), &reads);
  assert(first.has_value());

  auto duplicate = Open(Options(name, telepath::SharedMemoryMode::kCreate), root.path().string(), &reads);
  assert(!duplicate.has_value() && duplicate.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto other_size = Open(Options(name, telepath::SharedMemoryMode::kAttach, kPoolSize * 2), root.path().string(), &reads);
  assert(!other_size.has_value() && other_size.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto other_replacer = Open(Options(name, telepath::SharedMemoryMode::kAttach), root.path().string(), &reads, telepath::MakeClockReplacer(kPoolSize));
  assert(!other_replacer.has_value() && other_replacer.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto second = Open(Options(name, telepath::SharedMemoryMode::kCreateOrAttach), root.path().string(), &reads);
  assert(second.has_value());
  WriteByte(first->get(), 3, std::byte{0x33});
  assert(ReadByte(second->get(), 3) == std::byte{0x33});

  // The name goes away with the last attached manager.
  first->reset();
  assert(Open(Options(name, telepath::SharedMemoryMode::kCreate), root.path().string(), &reads).error().code() == telepath::ErrorCode::kInvalidArgument);
  second->reset();
  assert(Open(Options(name, telepath::SharedMemoryMode::kAttach), root.path().string(), &reads).error().code() == telepath::ErrorCode::kNotFound);
}

void ExpectPersistentSegmentNeedsRemoval() {
  const std::string name = SegmentName("shared_persistent");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_persistent");
  std::atomic<int> reads{0};
  auto options = Options(name, telepath::SharedMemoryMode::kCreate);
  options.shared_memory.remove_when_unused = false;
  {
    auto manager = Open(options, root.path().string(), &reads);
    assert(manager.has_value());
    WriteByte(manager->get(), 4, std::byte{0x44});
  }
  options.shared_memory.mode = telepath::SharedMemoryMode::kAttach;
  auto again = Open(options, root.path().string(), &reads);
  assert(again.has_value());
  assert(ReadByte(again->get(), 4) == std::byte{0x44});
  again->reset();

  assert(telepath::BufferManager::RemoveSharedMemory(name).has_value());
  auto removed = telepath::BufferManager::RemoveSharedMemory(name);
  assert(!removed.has_value() && removed.error().code() == telepath::ErrorCode::kNotFound);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 4 && std::strcmp(argv[1], "child-sees-parent-page") == 0) return ChildSeesParentPage(argv[2], argv[3]);
  if (argc == 4 && std::strcmp(argv[1], "child-stress") == 0) return ChildStress(argv[2], argv[3]);

  ExpectPagesAreSharedBetweenProcesses();
  ExpectConcurrentProcessesKeepPagesIntact();
  ExpectModesAndValidation();
  ExpectPersistentSegmentNeedsRemoval();
  return 0;
}
