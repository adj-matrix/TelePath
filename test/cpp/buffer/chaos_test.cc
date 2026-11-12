// Randomized concurrent workload over most of the BufferManager API with
// injected I/O faults, checked against a model of the expected page contents.
// Cross-process configurations run half of the workers in a second process
// that shares the cache and the model.
// Set TELEPATH_CHAOS_SEED to vary the run, TELEPATH_CHAOS_CONFIG to run one
// configuration, and TELEPATH_CHAOS_SKIP to a list of operation names (see
// kOperations) to leave out, which helps to narrow down a failure.
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <utility>

#include "buffer_test_support.h"
#include "fault_injection_backend.h"
#include "telepath/buffer/buffer_access_strategy.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/buffer/read_buffers_operation.h"
#include "telepath/io/disk_backend_factory.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

extern char **environ;

namespace {

using telepath::DiskOperation;
using telepath::ErrorCode;

constexpr std::size_t kPageSize = 4096;
constexpr telepath::FileId kFileCount = 3;
constexpr telepath::BlockId kBlocksPerFile = 24;
// Pages of this file are dropped while others read them, so only their
// internal consistency is checked.
constexpr telepath::FileId kScratchFile = 9;
constexpr telepath::BlockId kScratchBlocks = 8;

struct Config {
  const char *name;
  const char *replacer;
  std::size_t pool_size;
  std::size_t threads;
  std::size_t ops_per_thread;
  telepath::ForegroundWriteback foreground;
  bool cleaner;
  unsigned read_faults;
  unsigned write_faults;
  unsigned sync_faults;
  bool cross_process{false};
  // Run on io_uring instead of the POSIX backend; skipped where io_uring is
  // unavailable unless TELEPATH_REQUIRE_IO_URING_SUCCESS is set.
  bool io_uring{false};
  // Spread the workers over this many namespaces with quotas (0: only the
  // default namespace).
  std::size_t namespaces{0};
};

// Reads through a namespace when the worker has one, else through the
// manager's default namespace.
struct Reader {
  telepath::BufferManager *manager;
  std::optional<telepath::BufferNamespace> space;

  auto ReadBuffer(telepath::FileId file, telepath::BlockId block, telepath::BufferAccessStrategy *strategy = nullptr) {
    return space ? space->ReadBuffer(file, block, strategy) : manager->ReadBuffer(file, block, strategy);
  }
  auto ReadBufferExclusive(telepath::FileId file, telepath::BlockId block, telepath::BufferAccessStrategy *strategy = nullptr, telepath::ReadBufferMode mode = telepath::ReadBufferMode::kNormal) {
    return space ? space->ReadBufferExclusive(file, block, strategy, mode) : manager->ReadBufferExclusive(file, block, strategy, mode);
  }
  auto StartReadBuffers(telepath::ReadBuffersOperation &operation, telepath::FileId file, telepath::BlockId block, std::size_t count) {
    return space ? space->StartReadBuffers(operation, file, block, count) : manager->StartReadBuffers(operation, file, block, count);
  }
  auto PrefetchBuffer(telepath::FileId file, telepath::BlockId block, std::size_t count) {
    return space ? space->PrefetchBuffer(file, block, count) : manager->PrefetchBuffer(file, block, count);
  }
};

auto MakeBackend(const std::filesystem::path &root, bool io_uring) -> std::unique_ptr<telepath::DiskBackend> {
  if (!io_uring) return std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<kPageSize>());
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  options.allow_fallback = false;
  auto backend = telepath::CreateDiskBackend(root.string(), kPageSize, options);
  if (!backend.has_value()) return nullptr;
  return std::move(backend.value());
}

auto MakeReplacer(const std::string &name, std::size_t capacity) -> std::unique_ptr<telepath::Replacer> {
  if (name == "clock") return telepath::MakeClockReplacer(capacity);
  if (name == "clock_sweep") return telepath::MakeClockSweepReplacer(capacity);
  if (name == "sieve") return telepath::MakeSieveReplacer(capacity);
  if (name == "lru") return telepath::MakeLruReplacer(capacity);
  if (name == "two_queue") return telepath::MakeTwoQueueReplacer(capacity);
  return telepath::MakeLruKReplacer(capacity, 2);
}

// Page layout: an 8-byte write counter followed by its low byte repeated.
void WritePattern(std::span<std::byte> page, std::uint64_t counter) {
  std::memcpy(page.data(), &counter, sizeof(counter));
  std::memset(page.data() + sizeof(counter), static_cast<int>(counter & 0xFF), kPageSize - sizeof(counter));
}

auto CheckPattern(std::span<const std::byte> page, const char *what) -> std::uint64_t {
  std::uint64_t counter = 0;
  std::memcpy(&counter, page.data(), sizeof(counter));
  const auto fill = static_cast<std::byte>(counter & 0xFF);
  for (std::size_t i = sizeof(counter); i < kPageSize; ++i) {
    if (page[i] != fill) {
      std::fprintf(stderr, "%s: torn page with counter %llu at byte %zu\n", what, static_cast<unsigned long long>(counter), i);
      std::abort();
    }
  }
  return counter;
}

bool Tolerated(const telepath::Error &error) { return error.code() == ErrorCode::kIoError || error.code() == ErrorCode::kResourceExhausted; }

void Require(const telepath::Status &status, const Config &config, const char *what) {
  if (status.has_value() || Tolerated(status.error())) return;
  std::fprintf(stderr, "%s: %s failed with code %d: %s\n", config.name, what, static_cast<int>(status.error().code()), status.error().message());
  std::abort();
}

thread_local const char *stage = "worker";

const char *const kOperations[] = {"read", "write", "readbuffers", "prefetch", "flush", "flushfile", "syncfile", "flushall", "checkpoint", "scratch", "drop", "zero", "bulk"};

auto Skipped(const char *operation) -> bool {
  const char *skip = std::getenv("TELEPATH_CHAOS_SKIP");
  if (skip == nullptr) return false;
  const std::string list = std::string(",") + skip + ",";
  return list.find(std::string(",") + operation + ",") != std::string::npos;
}

// Pages start at this counter, so a page read back as zeros stands out.
constexpr std::uint64_t kInitialCounter = 1000;

// Expected page counters and the next LSN, in memory that a second process
// can map as well.
class Model {
 public:
  static constexpr std::size_t kSlots = kFileCount * kBlocksPerFile + 1;
  static constexpr std::size_t kBytes = kSlots * sizeof(std::atomic<std::uint64_t>);

  // Creates the model, in a shared-memory object when `name` is not empty.
  explicit Model(std::string name = {}) : name_(std::move(name)) {
    void *memory = nullptr;
    if (name_.empty()) {
      memory = ::operator new(kBytes);
    } else {
      const int fd = shm_open(name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
      assert(fd >= 0 && ftruncate(fd, kBytes) == 0);
      memory = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      close(fd);
      assert(memory != MAP_FAILED);
    }
    slots_ = static_cast<std::atomic<std::uint64_t> *>(memory);
    for (std::size_t slot = 0; slot < kSlots; ++slot) std::construct_at(&slots_[slot], kInitialCounter);
    slots_[kSlots - 1].store(1);
  }

  // Maps a model another process created.
  static auto Attach(const std::string &name) -> Model {
    const int fd = shm_open(name.c_str(), O_RDWR, 0);
    assert(fd >= 0);
    void *memory = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    assert(memory != MAP_FAILED);
    return Model(static_cast<std::atomic<std::uint64_t> *>(memory));
  }

  Model(Model &&other) noexcept : name_(std::move(other.name_)), slots_(std::exchange(other.slots_, nullptr)), owner_(other.owner_), mapped_(other.mapped_) {}
  Model(const Model &) = delete;
  Model &operator=(const Model &) = delete;
  ~Model() {
    if (slots_ == nullptr) return;
    if (name_.empty() && !mapped_) {
      ::operator delete(slots_);
      return;
    }
    munmap(slots_, kBytes);
    if (owner_) shm_unlink(name_.c_str());
  }

  auto at(telepath::FileId file, telepath::BlockId block) -> std::atomic<std::uint64_t> & { return slots_[file * kBlocksPerFile + block]; }
  auto next_lsn() -> std::atomic<std::uint64_t> & { return slots_[kSlots - 1]; }

 private:
  explicit Model(std::atomic<std::uint64_t> *slots) : slots_(slots), owner_(false), mapped_(true) {}

  std::string name_;
  std::atomic<std::uint64_t> *slots_{nullptr};
  bool owner_{true};
  bool mapped_{false};
};

void CheckCounter(std::span<const std::byte> page, Model &model, telepath::FileId file, telepath::BlockId block, const Config &config) {
  const std::uint64_t seen = CheckPattern(page, config.name);
  const std::uint64_t expected = model.at(file, block).load();
  if (seen == expected) return;
  std::fprintf(stderr, "%s (%s): page %u/%llu holds %llu, expected %llu\n", config.name, stage, file, static_cast<unsigned long long>(block),
               static_cast<unsigned long long>(seen), static_cast<unsigned long long>(expected));
  std::abort();
}

void RunWorker(telepath::BufferManager *manager, std::optional<telepath::BufferNamespace> space, Model *model, const Config &config, std::uint64_t seed) {
  Reader reader{manager, space};
  std::mt19937_64 rng(seed);
  auto pick = [&rng](std::uint64_t bound) { return std::uniform_int_distribution<std::uint64_t>(0, bound - 1)(rng); };
  telepath::BufferAccessStrategy bulk(telepath::BufferAccessStrategyType::kBulkRead, 2);

  for (std::size_t op = 0; op < config.ops_per_thread; ++op) {
    const auto file = static_cast<telepath::FileId>(pick(kFileCount));
    const telepath::BlockId block = pick(kBlocksPerFile);
    const std::uint64_t kind = pick(1000);

    const char *name = kind < 60 ? "bulk" : kind < 300 ? "read" : kind < 330 ? "zero" : kind < 550 ? "write" : kind < 650 ? "readbuffers" : kind < 720 ? "prefetch"
      : kind < 800 ? "flush" : kind < 830 ? "flushfile" : kind < 850 ? "syncfile" : kind < 865 ? "flushall" : kind < 885 ? "checkpoint" : kind < 960 ? "scratch" : "drop";
    if (Skipped(name)) continue;
    if (kind < 300) {
      telepath::BufferAccessStrategy *strategy = kind < 60 ? &bulk : nullptr;
      auto handle = reader.ReadBuffer(file, block, strategy);
      if (!handle.has_value()) {
        Require(std::unexpected(handle.error()), config, "ReadBuffer");
        continue;
      }
      CheckCounter(handle->data(), *model, file, block, config);
    } else if (kind < 550) {
      const bool overwrite = kind < 330;
      auto handle = reader.ReadBufferExclusive(file, block, nullptr, overwrite ? telepath::ReadBufferMode::kZeroAndLock : telepath::ReadBufferMode::kNormal);
      if (!handle.has_value()) {
        Require(std::unexpected(handle.error()), config, "ReadBufferExclusive");
        continue;
      }
      std::atomic<std::uint64_t> &expected = model->at(file, block);
      if (!overwrite) CheckCounter(handle->data(), *model, file, block, config);
      WritePattern(handle->mutable_data(), expected.load() + 1);
      if (kind % 2 == 0) handle->MarkDirty(model->next_lsn().fetch_add(1));
      expected.fetch_add(1);
      if (kind >= 540) Require(manager->FlushBuffer(*handle), config, "FlushBuffer(handle)");
    } else if (kind < 650) {
      const std::size_t count = pick(kBlocksPerFile + 8);
      const telepath::BlockId first = pick(kBlocksPerFile);
      const std::size_t clamped = std::min<std::size_t>(count, kBlocksPerFile - first);
      telepath::ReadBuffersOperation operation;
      telepath::Status started = reader.StartReadBuffers(operation, file, first, clamped);
      Require(started, config, "StartReadBuffers");
      if (!started.has_value()) continue;
      telepath::Status waited = manager->WaitReadBuffers(operation);
      Require(waited, config, "WaitReadBuffers");
      if (!waited.has_value()) {
        assert(operation.handles().empty());
        continue;
      }
      assert(operation.handles().size() == clamped);
      for (std::size_t i = 0; i < clamped; ++i) CheckCounter(operation.handles()[i].data(), *model, file, first + i, config);
    } else if (kind < 720) {
      const telepath::BlockId first = pick(kBlocksPerFile);
      Require(reader.PrefetchBuffer(file, first, 1 + pick(kBlocksPerFile - first)), config, "PrefetchBuffer");
    } else if (kind < 800) {
      Require(manager->FlushBuffer(file, block), config, "FlushBuffer");
    } else if (kind < 830) {
      Require(manager->FlushFileBuffers(file), config, "FlushFileBuffers");
    } else if (kind < 850) {
      Require(manager->SyncFile(file), config, "SyncFile");
    } else if (kind < 865) {
      Require(manager->FlushAll(), config, "FlushAll");
    } else if (kind < 885) {
      Require(manager->CheckpointBuffers(model->next_lsn().load()), config, "CheckpointBuffers");
    } else if (kind < 960) {
      const telepath::BlockId scratch = pick(kScratchBlocks);
      if (kind < 920) {
        auto handle = reader.ReadBufferExclusive(kScratchFile, scratch);
        if (!handle.has_value()) {
          Require(std::unexpected(handle.error()), config, "scratch write");
          continue;
        }
        WritePattern(handle->mutable_data(), CheckPattern(handle->data(), config.name) + 1);
      } else {
        auto handle = reader.ReadBuffer(kScratchFile, scratch);
        if (!handle.has_value()) {
          Require(std::unexpected(handle.error()), config, "scratch read");
          continue;
        }
        CheckPattern(handle->data(), config.name);
      }
    } else {
      telepath::Status dropped = manager->DropFileBuffers(kScratchFile, pick(kScratchBlocks));
      if (!dropped.has_value() && dropped.error().code() != ErrorCode::kInvalidArgument) Require(dropped, config, "DropFileBuffers");
    }
  }
}

// Namespaces with a guarantee, a cap, and neither, over the default one;
// declared at creation, as a cross-process cache requires.
auto Declarations(const Config &config) -> std::vector<telepath::NamespaceOptions> {
  std::vector<telepath::NamespaceOptions> spaces;
  for (std::size_t index = 0; index < config.namespaces; ++index) {
    telepath::NamespaceOptions options;
    options.name = "space" + std::to_string(index);
    options.replacer = MakeReplacer(index % 2 == 0 ? "lru" : "sieve", config.pool_size);
    options.min_frames = index == 0 ? config.pool_size / 4 : 0;
    options.max_frames = index == 1 ? config.pool_size / 2 : 0;
    spaces.push_back(std::move(options));
  }
  return spaces;
}

auto Open(const std::filesystem::path &root, const Config &config, telepath::test_support::FaultInjectionBackend **faults, std::uint64_t seed, const std::string &segment = {}, telepath::SharedMemoryMode mode = telepath::SharedMemoryMode::kCreate) -> std::unique_ptr<telepath::BufferManager> {
  telepath::BufferManagerOptions options;
  options.pool_size = config.pool_size;
  options.page_size = kPageSize;
  options.foreground_writeback = config.foreground;
  options.enable_background_cleaner = config.cleaner;
  options.shared_memory.name = segment;
  options.shared_memory.mode = mode;
  auto inner = MakeBackend(root, config.io_uring);
  assert(inner != nullptr);
  auto backend = std::make_unique<telepath::test_support::FaultInjectionBackend>(std::move(inner), seed);
  if (faults != nullptr) *faults = backend.get();
  auto manager = telepath::BufferManager::Create(options, std::move(backend), MakeReplacer(config.replacer, config.pool_size), telepath::MakeNoOpTelemetrySink(), Declarations(config));
  assert(manager.has_value());
  return std::move(manager.value());
}

void EnableFaults(telepath::test_support::FaultInjectionBackend *faults, const Config &config) {
  if (std::getenv("TELEPATH_CHAOS_NO_FAULTS") != nullptr) return;
  faults->SetFaultRate(DiskOperation::kRead, config.read_faults);
  faults->SetFaultRate(DiskOperation::kWrite, config.write_faults);
  faults->SetFaultRate(DiskOperation::kSync, config.sync_faults);
}

void RunWorkers(telepath::BufferManager *manager, Model *model, const Config &config, std::uint64_t seed) {
  std::vector<telepath::BufferNamespace> spaces;
  for (std::size_t index = 0; index < config.namespaces; ++index) {
    auto found = manager->GetNamespace("space" + std::to_string(index));
    assert(found.has_value() && found->id() == index + 1);
    spaces.push_back(found.value());
  }
  std::vector<std::thread> workers;
  for (std::size_t worker = 0; worker < config.threads; ++worker) {
    std::optional<telepath::BufferNamespace> space;
    if (!spaces.empty() && worker % (spaces.size() + 1) != 0) space = spaces[worker % (spaces.size() + 1) - 1];
    workers.emplace_back(RunWorker, manager, space, model, std::cref(config), seed * 7919 + worker);
  }
  for (auto &worker : workers) worker.join();
}

// The second process of a cross-process configuration.
auto RunChild(const Config &config, std::uint64_t seed, const std::string &segment, const std::string &model_name, const std::string &root) -> int {
  Model model = Model::Attach(model_name);
  telepath::test_support::FaultInjectionBackend *faults = nullptr;
  auto manager = Open(root, config, &faults, seed + 1, segment, telepath::SharedMemoryMode::kAttach);
  EnableFaults(faults, config);
  RunWorkers(manager.get(), &model, config, seed * 31 + 17);
  faults->ClearFaults();
  return manager->failure().has_value() ? 3 : 0;
}

auto SpawnChild(const Config &config, std::uint64_t seed, const std::string &segment, const std::string &model_name, const std::string &root) -> pid_t {
  const std::string seed_text = std::to_string(seed);
  std::vector<char *> argv{const_cast<char *>("/proc/self/exe"), const_cast<char *>("chaos-child"), const_cast<char *>(config.name),
                           const_cast<char *>(seed_text.c_str()), const_cast<char *>(segment.c_str()), const_cast<char *>(model_name.c_str()),
                           const_cast<char *>(root.c_str()), nullptr};
  pid_t pid = 0;
  assert(posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv.data(), environ) == 0);
  return pid;
}

void Run(const Config &config, std::uint64_t seed) {
  telepath::buffer_test_support::TestRootGuard root(std::string("telepath_chaos_") + config.name);
  if (config.io_uring && MakeBackend(root.path(), true) == nullptr) {
    assert(std::getenv("TELEPATH_REQUIRE_IO_URING_SUCCESS") == nullptr);
    std::fprintf(stderr, "%s: skipped, io_uring is unavailable\n", config.name);
    return;
  }
  const std::string suffix = std::string(config.name) + "_" + std::to_string(getpid());
  const std::string segment = config.cross_process ? "/telepath_chaos_" + suffix : std::string();
  const std::string model_name = config.cross_process ? "/telepath_chaos_model_" + suffix : std::string();
  Model model(model_name);
  {
    telepath::test_support::FaultInjectionBackend *faults = nullptr;
    auto manager = Open(root.path(), config, &faults, seed, segment);
    for (telepath::FileId file = 0; file < kFileCount; ++file) {
      for (telepath::BlockId block = 0; block < kBlocksPerFile; ++block) {
        auto handle = manager->ReadBufferExclusive(file, block, nullptr, telepath::ReadBufferMode::kZeroAndLock);
        assert(handle.has_value());
        WritePattern(handle->mutable_data(), kInitialCounter);
      }
    }
    for (telepath::BlockId block = 0; block < kScratchBlocks; ++block) {
      auto handle = manager->ReadBufferExclusive(kScratchFile, block, nullptr, telepath::ReadBufferMode::kZeroAndLock);
      assert(handle.has_value());
      WritePattern(handle->mutable_data(), 0);
    }
    assert(manager->FlushAll().has_value());

    const pid_t child = config.cross_process ? SpawnChild(config, seed, segment, model_name, root.path().string()) : -1;
    EnableFaults(faults, config);
    RunWorkers(manager.get(), &model, config, seed);
    faults->ClearFaults();
    if (child > 0) {
      int status = 0;
      assert(waitpid(child, &status, 0) == child);
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "%s: second process failed with status %d\n", config.name, status);
        std::abort();
      }
    }

    if (config.read_faults + config.write_faults + config.sync_faults != 0 && std::getenv("TELEPATH_CHAOS_NO_FAULTS") == nullptr) assert(faults->injected() > 0);
    assert(!manager->failure().has_value());
    // The dirty-page counter, which every path and process updates, must agree
    // with the frames' dirty bits once writeback settles; a lost or doubled
    // update would keep them apart forever.
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
      const auto snapshot = manager->ExportSnapshot();
      std::size_t dirty = 0;
      for (const auto &frame : snapshot.frames) dirty += frame.is_dirty ? 1 : 0;
      if (snapshot.flush_queued_count == 0 && snapshot.flush_in_flight_count == 0 && snapshot.dirty_page_count == dirty) break;
      if (std::chrono::steady_clock::now() > settle_deadline) {
        std::fprintf(stderr, "%s: dirty-page counter %zu, dirty frames %zu\n", config.name, snapshot.dirty_page_count, dirty);
        std::abort();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(manager->FlushAll().has_value());
    stage = "after run";
    for (telepath::FileId file = 0; file < kFileCount; ++file) {
      for (telepath::BlockId block = 0; block < kBlocksPerFile; ++block) {
        auto handle = manager->ReadBuffer(file, block);
        assert(handle.has_value());
        CheckCounter(handle->data(), model, file, block, config);
      }
    }
    const auto snapshot = manager->ExportSnapshot();
    assert(snapshot.dirty_page_count == 0 && snapshot.flush_queued_count == 0 && snapshot.flush_in_flight_count == 0);
    for (const auto &frame : snapshot.frames) assert(frame.pin_count == 0 && !frame.is_dirty && frame.rec_lsn == telepath::kInvalidLsn);
    assert(!manager->MinRecLsn().has_value());
    assert(manager->Close().has_value());
  }

  stage = "reopened";
  auto reopened = Open(root.path(), config, nullptr, seed);
  for (telepath::FileId file = 0; file < kFileCount; ++file) {
    for (telepath::BlockId block = 0; block < kBlocksPerFile; ++block) {
      auto handle = reopened->ReadBuffer(file, block);
      assert(handle.has_value());
      CheckCounter(handle->data(), model, file, block, config);
    }
  }
  for (telepath::BlockId block = 0; block < kScratchBlocks; ++block) {
    auto handle = reopened->ReadBuffer(kScratchFile, block);
    assert(handle.has_value());
    CheckPattern(handle->data(), config.name);
  }
}

}  // namespace

int main(int argc, char **argv) {
  std::uint64_t seed = 20261026;
  if (const char *value = std::getenv("TELEPATH_CHAOS_SEED")) seed = std::strtoull(value, nullptr, 10);

  using telepath::ForegroundWriteback;
  const Config configs[] = {
    {"faults_clock_sweep", "clock_sweep", 6, 6, 1500, ForegroundWriteback::kDirect, true, 20, 30, 50},
    {"faults_clock", "clock", 6, 6, 1500, ForegroundWriteback::kDirect, false, 20, 30, 50},
    {"faults_sieve", "sieve", 6, 6, 1500, ForegroundWriteback::kQueued, true, 20, 30, 50},
    {"faults_lru", "lru", 6, 6, 1500, ForegroundWriteback::kDirect, true, 20, 30, 50},
    {"faults_lru_k", "lru_k", 6, 6, 1500, ForegroundWriteback::kQueued, false, 20, 30, 50},
    {"faults_two_queue", "two_queue", 6, 6, 1500, ForegroundWriteback::kDirect, true, 20, 30, 50},
    {"one_frame", "clock_sweep", 1, 4, 800, ForegroundWriteback::kDirect, true, 20, 30, 50},
    {"two_frames_queued", "lru", 2, 4, 800, ForegroundWriteback::kQueued, true, 0, 50, 0},
    {"roomy_no_faults", "two_queue", 64, 8, 2000, ForegroundWriteback::kQueued, true, 0, 0, 0},
    {"cross_process_faults", "clock_sweep", 8, 4, 1500, ForegroundWriteback::kDirect, true, 20, 30, 50, true},
    {"cross_process_lru_k", "lru_k", 6, 4, 1500, ForegroundWriteback::kQueued, true, 20, 30, 50, true},
    {"io_uring_faults", "sieve", 6, 6, 1500, ForegroundWriteback::kQueued, true, 20, 30, 50, false, true},
    {"io_uring_cross_process", "clock_sweep", 8, 4, 1500, ForegroundWriteback::kDirect, true, 20, 30, 50, true, true},
    {"namespaces_faults", "clock_sweep", 12, 8, 1500, ForegroundWriteback::kDirect, true, 20, 30, 50, false, false, 3},
    {"namespaces_tight", "lru_k", 4, 6, 1200, ForegroundWriteback::kQueued, true, 0, 30, 0, false, false, 3},
    {"cross_process_namespaces", "clock_sweep", 12, 4, 1500, ForegroundWriteback::kDirect, true, 20, 30, 50, true, false, 3},
  };
  if (argc == 7 && std::string(argv[1]) == "chaos-child") {
    for (const Config &config : configs) {
      if (std::string(argv[2]) == config.name) return RunChild(config, std::strtoull(argv[3], nullptr, 10), argv[4], argv[5], argv[6]);
    }
    return 2;
  }
  std::fprintf(stderr, "chaos seed %llu\n", static_cast<unsigned long long>(seed));
  const char *only = std::getenv("TELEPATH_CHAOS_CONFIG");
  for (const Config &config : configs) {
    if (only == nullptr || std::string(only) == config.name) Run(config, seed);
  }
  return 0;
}
