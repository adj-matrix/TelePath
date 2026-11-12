// Namespaces of a cross-process cache: declared when the segment is created,
// identically in every process, with quotas counted and replacer state kept in
// the segment, so every process enforces the same limits and evicts by the
// same history. Children re-execute this binary through /proc/self/exe.
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/facade/ml_prefetch_buffer.h"
#include "telepath/facade/page_cache.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

extern char **environ;

namespace {

using telepath::ErrorCode;
using telepath::NamespaceId;
using telepath::SharedMemoryMode;

constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPoolSize = 16;
constexpr telepath::FileId kFilesFile = 1;
constexpr telepath::FileId kScanFile = 2;
constexpr telepath::FileId kHotFile = 3;
constexpr telepath::FileId kDefaultFile = 4;
constexpr NamespaceId kFiles = 1;
constexpr NamespaceId kScan = 2;
constexpr NamespaceId kHot = 3;

// Forwards to a PosixDiskBackend and counts block reads.
class CountingBackend : public telepath::DiskBackend {
 public:
  CountingBackend(const std::string &root, std::atomic<int> *reads) : inner_(root, telepath::BlockGeometry::Of<kPageSize>()), reads_(reads) {}
  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    for (const auto &request : requests) {
      if (request.operation == telepath::DiskOperation::kRead) reads_->fetch_add(static_cast<int>(request.block_count));
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

// A policy that keeps its state on the heap only, like a custom replacer that
// does not implement SharedStateBytes().
class HeapOnlyReplacer : public telepath::Replacer {
 public:
  HeapOnlyReplacer() : inner_(telepath::MakeLruReplacer(kPoolSize)) {}
  void RecordAccess(telepath::FrameId frame_id) override { inner_->RecordAccess(frame_id); }
  void RecordLoad(telepath::FrameId frame_id) override { inner_->RecordLoad(frame_id); }
  bool Victim(const telepath::EvictionProbe &probe, telepath::FrameId *frame_id) override { return inner_->Victim(probe, frame_id); }
  void Remove(telepath::FrameId frame_id) override { inner_->Remove(frame_id); }

 private:
  std::unique_ptr<telepath::Replacer> inner_;
};

auto Space(std::string name, std::unique_ptr<telepath::Replacer> replacer, std::size_t min_frames = 0, std::size_t max_frames = 0, telepath::PreWriteHook hook = {}) -> telepath::NamespaceOptions {
  telepath::NamespaceOptions options;
  options.name = std::move(name);
  options.replacer = std::move(replacer);
  options.min_frames = min_frames;
  options.max_frames = max_frames;
  options.pre_write_hook = std::move(hook);
  return options;
}

// The declaration every process of these tests agrees on: a file cache with a
// guarantee, a capped scan, and a capped SIEVE namespace whose eviction order
// is exact.
auto Declarations(telepath::PreWriteHook files_hook = {}) -> std::vector<telepath::NamespaceOptions> {
  std::vector<telepath::NamespaceOptions> spaces;
  spaces.push_back(Space("files", telepath::MakeLruReplacer(kPoolSize), 6, 0, std::move(files_hook)));
  spaces.push_back(Space("scan", telepath::MakeClockSweepReplacer(kPoolSize), 0, 4));
  spaces.push_back(Space("hot", telepath::MakeSieveReplacer(kPoolSize), 0, 4));
  return spaces;
}

auto Open(const std::string &name, SharedMemoryMode mode, const std::string &root, std::atomic<int> *reads, std::vector<telepath::NamespaceOptions> spaces,
          telepath::PreWriteHook default_hook = {}) -> telepath::Result<std::unique_ptr<telepath::BufferManager>> {
  telepath::BufferManagerOptions options;
  options.pool_size = kPoolSize;
  options.page_size = kPageSize;
  options.shared_memory.name = name;
  options.shared_memory.mode = mode;
  options.pre_write_hook = std::move(default_hook);
  return telepath::BufferManager::Create(options, std::make_unique<CountingBackend>(root, reads), telepath::MakeClockSweepReplacer(kPoolSize), telepath::MakeNoOpTelemetrySink(), std::move(spaces));
}

auto SegmentName(const char *test) -> std::string { return std::string("/telepath_ns_") + test + "_" + std::to_string(getpid()); }

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

// Frames each namespace holds, by namespace id; index kMaxNamespaces counts
// free frames.
auto Occupancy(telepath::BufferManager &manager) -> std::vector<std::size_t> {
  std::vector<std::size_t> counts(telepath::kMaxNamespaces + 1, 0);
  for (const auto &frame : manager.ExportSnapshot().frames) {
    if (frame.namespace_id == telepath::kNoNamespace) ++counts[telepath::kMaxNamespaces];
    else ++counts[frame.namespace_id];
  }
  return counts;
}

auto Resident(telepath::BufferManager &manager) -> std::set<std::pair<telepath::FileId, telepath::BlockId>> {
  std::set<std::pair<telepath::FileId, telepath::BlockId>> pages;
  for (const auto &frame : manager.ExportSnapshot().frames) {
    if (frame.is_valid) pages.insert({frame.tag.file_id, frame.tag.block_id});
  }
  return pages;
}

auto Read(telepath::BufferNamespace space, telepath::FileId file, telepath::BlockId block) -> bool { return space.ReadBuffer(file, block).has_value(); }

// The error code of a failed result; a successful one fails the test.
auto Code(const auto &result) -> ErrorCode {
  assert(!result.has_value());
  return result.error().code();
}

// Every invalid declaration fails before a segment exists, so nothing is left
// to attach to; a valid one is reachable by name, and a cross-process cache
// cannot add namespaces after creation.
void ExpectDeclarationsAreValidated() {
  const std::string name = SegmentName("validate");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_namespace_validate");
  std::atomic<int> reads{0};

  auto expect_refused = [&](std::vector<telepath::NamespaceOptions> spaces, ErrorCode code) {
    auto refused = Open(name, SharedMemoryMode::kCreate, root.path().string(), &reads, std::move(spaces));
    assert(Code(refused) == code);
    assert(Code(Open(name, SharedMemoryMode::kAttach, root.path().string(), &reads, {})) == ErrorCode::kNotFound);
  };
  auto with = [](telepath::NamespaceOptions extra) {
    auto spaces = Declarations();
    spaces.push_back(std::move(extra));
    return spaces;
  };
  expect_refused(with(Space("files", telepath::MakeLruReplacer(kPoolSize))), ErrorCode::kInvalidArgument);
  expect_refused(with(Space("default", telepath::MakeLruReplacer(kPoolSize))), ErrorCode::kInvalidArgument);
  expect_refused(with(Space("", telepath::MakeLruReplacer(kPoolSize))), ErrorCode::kInvalidArgument);
  expect_refused(with(Space("null", nullptr)), ErrorCode::kInvalidArgument);
  expect_refused(with(Space("inverted", telepath::MakeLruReplacer(kPoolSize), 3, 2)), ErrorCode::kInvalidArgument);
  expect_refused(with(Space("greedy", telepath::MakeLruReplacer(kPoolSize), kPoolSize - 5)), ErrorCode::kInvalidArgument);
  expect_refused(with(Space("heap", std::make_unique<HeapOnlyReplacer>())), ErrorCode::kInvalidArgument);
  expect_refused(with(Space(std::string(64, 'n'), telepath::MakeLruReplacer(kPoolSize))), ErrorCode::kInvalidArgument);
  {
    std::vector<telepath::NamespaceOptions> spaces;
    for (std::size_t index = 0; index < telepath::kMaxNamespaces; ++index) spaces.push_back(Space("space" + std::to_string(index), telepath::MakeLruReplacer(kPoolSize)));
    expect_refused(std::move(spaces), ErrorCode::kResourceExhausted);
  }

  // The longest name that fits, and the most namespaces that fit.
  std::vector<telepath::NamespaceOptions> spaces;
  for (std::size_t index = 1; index + 1 < telepath::kMaxNamespaces; ++index) spaces.push_back(Space("space" + std::to_string(index), telepath::MakeLruReplacer(kPoolSize)));
  spaces.push_back(Space(std::string(63, 'n'), telepath::MakeSieveReplacer(kPoolSize)));
  auto manager = Open(name, SharedMemoryMode::kCreate, root.path().string(), &reads, std::move(spaces));
  assert(manager.has_value());
  assert(manager.value()->GetNamespace(std::string(63, 'n')).value().id() == telepath::kMaxNamespaces - 1);
  assert(manager.value()->GetNamespace("space3").value().id() == 3);
  assert(manager.value()->GetNamespace("default").value().id() == telepath::kDefaultNamespace);
  assert(Code(manager.value()->GetNamespace("space99")) == ErrorCode::kNotFound);
  assert(Code(manager.value()->GetNamespace("")) == ErrorCode::kNotFound);
  auto late = manager.value()->CreateNamespace(Space("late", telepath::MakeLruReplacer(kPoolSize)));
  assert(Code(late) == ErrorCode::kUnavailable);
  telepath::NamespaceOptions cache_space;
  cache_space.name = "cache";
  assert(Code(telepath::PageCache::Create({}, *manager.value(), std::move(cache_space))) == ErrorCode::kUnavailable);
  telepath::NamespaceOptions dataset_space;
  dataset_space.name = "dataset";
  assert(Code(telepath::MLPrefetchBuffer::Create({}, *manager.value(), std::move(dataset_space))) == ErrorCode::kUnavailable);
  assert(manager.value()->GetNamespace("cache").error().code() == ErrorCode::kNotFound);
  manager->reset();

  // A private cache accepts the same declarations, under the same rules, and
  // still adds namespaces later.
  telepath::BufferManagerOptions options;
  options.pool_size = kPoolSize;
  options.page_size = kPageSize;
  auto duplicate = telepath::BufferManager::Create(options, std::make_unique<CountingBackend>(root.path().string(), &reads), telepath::MakeClockSweepReplacer(kPoolSize),
                                                   telepath::MakeNoOpTelemetrySink(), with(Space("scan", telepath::MakeLruReplacer(kPoolSize))));
  assert(Code(duplicate) == ErrorCode::kInvalidArgument);
  auto heap = with(Space("heap", std::make_unique<HeapOnlyReplacer>()));
  auto local = telepath::BufferManager::Create(options, std::make_unique<CountingBackend>(root.path().string(), &reads), telepath::MakeClockSweepReplacer(kPoolSize),
                                               telepath::MakeNoOpTelemetrySink(), std::move(heap));
  assert(local.has_value());
  assert(local.value()->GetNamespace("hot").value().id() == kHot);
  assert(local.value()->GetNamespace("heap").value().id() == 4);
  auto added = local.value()->CreateNamespace(Space("late", telepath::MakeLruReplacer(kPoolSize)));
  assert(added.has_value() && added->id() == 5);
  assert(local.value()->GetNamespace("late").value().id() == 5);
}

// Attaching validates the whole declaration: names, order, quotas, and
// replacer kinds. Pre-write hooks belong to each process and may differ.
void ExpectAttachersMustDeclareTheSameNamespaces() {
  const std::string name = SegmentName("attach");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_namespace_attach");
  std::atomic<int> reads{0};
  auto creator = Open(name, SharedMemoryMode::kCreate, root.path().string(), &reads, Declarations());
  assert(creator.has_value());
  assert(Read(creator.value()->GetNamespace("files").value(), kFilesFile, 0));

  auto expect_mismatch = [&](std::vector<telepath::NamespaceOptions> spaces, SharedMemoryMode mode = SharedMemoryMode::kAttach) {
    assert(Code(Open(name, mode, root.path().string(), &reads, std::move(spaces))) == ErrorCode::kInvalidArgument);
  };
  expect_mismatch({});
  {
    auto spaces = Declarations();
    spaces.pop_back();
    expect_mismatch(std::move(spaces));
  }
  {
    auto spaces = Declarations();
    spaces.push_back(Space("extra", telepath::MakeLruReplacer(kPoolSize)));
    expect_mismatch(std::move(spaces));
  }
  {
    auto spaces = Declarations();
    std::swap(spaces[1], spaces[2]);
    expect_mismatch(std::move(spaces));
  }
  {
    auto spaces = Declarations();
    spaces[0].min_frames = 5;
    expect_mismatch(std::move(spaces));
  }
  {
    auto spaces = Declarations();
    spaces[1].max_frames = 5;
    expect_mismatch(std::move(spaces));
  }
  {
    auto spaces = Declarations();
    spaces[2].replacer = telepath::MakeLruReplacer(kPoolSize);
    expect_mismatch(std::move(spaces), SharedMemoryMode::kCreateOrAttach);
  }
  {
    auto spaces = Declarations();
    spaces[2].name = "cold";
    expect_mismatch(std::move(spaces));
  }

  int hook_calls = 0;
  auto attacher = Open(name, SharedMemoryMode::kAttach, root.path().string(), &reads, Declarations([&](const telepath::BufferTag &, std::span<const std::byte>) {
    ++hook_calls;
    return telepath::Ok();
  }));
  assert(attacher.has_value());
  assert(attacher.value()->GetNamespace("hot").value().id() == kHot);
  const auto occupancy = Occupancy(*attacher.value());
  assert(occupancy[kFiles] == 1 && occupancy[telepath::kMaxNamespaces] == kPoolSize - 1);
}

// Child: drives one quota after another from the second process. Exit codes
// name the step that failed.
auto QuotaChild(const std::string &name, const std::string &root) -> int {
  std::atomic<int> reads{0};
  auto opened = Open(name, SharedMemoryMode::kAttach, root, &reads, Declarations());
  if (!opened.has_value()) return 2;
  telepath::BufferManager &manager = *opened.value();
  // The parent's scan pages count toward the cap, so the scan keeps reusing
  // its own four frames and leaves the free ones alone.
  const telepath::BufferNamespace scan = manager.GetNamespace("scan").value();
  for (telepath::BlockId block = 4; block < 34; ++block) {
    if (!Read(scan, kScanFile, block)) return 3;
  }
  auto occupancy = Occupancy(manager);
  if (occupancy[kScan] != 4 || occupancy[telepath::kMaxNamespaces] != 6) return 4;
  for (telepath::BlockId block = 0; block < 6; ++block) {
    if (!Read(manager.DefaultNamespace(), kDefaultFile, block)) return 5;
  }
  // The parent's file pages fill the guarantee, so the file cache takes one
  // frame from the namespace furthest above its guarantee and then reuses
  // its own.
  const telepath::BufferNamespace files = manager.GetNamespace("files").value();
  for (telepath::BlockId block = 6; block < 16; ++block) {
    if (!Read(files, kFilesFile, block)) return 6;
  }
  occupancy = Occupancy(manager);
  if (occupancy[kFiles] != 7 || occupancy[kScan] != 4 || occupancy[telepath::kDefaultNamespace] != 5) return 7;
  return 0;
}

// Caps and guarantees count every process's frames: a per-process count
// would let the child's scan take the free frames and its file cache keep
// taking frames from others.
void ExpectQuotasHoldAcrossProcesses() {
  const std::string name = SegmentName("quota");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_namespace_quota");
  std::atomic<int> reads{0};
  auto manager = Open(name, SharedMemoryMode::kCreate, root.path().string(), &reads, Declarations());
  assert(manager.has_value());
  const telepath::BufferNamespace files = manager.value()->GetNamespace("files").value();
  const telepath::BufferNamespace scan = manager.value()->GetNamespace("scan").value();
  for (telepath::BlockId block = 0; block < 6; ++block) assert(Read(files, kFilesFile, block));
  for (telepath::BlockId block = 0; block < 4; ++block) assert(Read(scan, kScanFile, block));

  assert(RunChild({"quota-child", name, root.path().string()}) == 0);

  const auto occupancy = Occupancy(*manager.value());
  assert(occupancy[kFiles] == 7 && occupancy[kScan] == 4 && occupancy[telepath::kDefaultNamespace] == 5 && occupancy[telepath::kMaxNamespaces] == 0);
  // The parent's own count agrees: its scan is still at the cap and evicts
  // only its own pages.
  const auto before = Resident(*manager.value());
  assert(Read(scan, kScanFile, 100));
  const auto after = Occupancy(*manager.value());
  assert(after[kScan] == 4 && after[kFiles] == 7 && after[telepath::kDefaultNamespace] == 5);
  for (const auto &page : before) {
    if (page.first != kScanFile) assert(Resident(*manager.value()).contains(page));
  }
}

// Child: hits one page of the SIEVE namespace through its own default
// namespace, which must mark it in the shared policy state.
auto HitChild(const std::string &name, const std::string &root, telepath::BlockId block) -> int {
  std::atomic<int> reads{0};
  auto opened = Open(name, SharedMemoryMode::kAttach, root, &reads, Declarations());
  if (!opened.has_value()) return 2;
  if (!Read(opened.value()->DefaultNamespace(), kHotFile, block)) return 3;
  if (reads.load() != 0) return 4;
  return 0;
}

// A namespace's replacer state is shared: a hit in another process protects a
// page from the next eviction exactly as a local hit would.
void ExpectReplacerStateIsShared() {
  const std::string name = SegmentName("replacer");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_namespace_replacer");
  std::atomic<int> reads{0};
  auto manager = Open(name, SharedMemoryMode::kCreate, root.path().string(), &reads, Declarations());
  assert(manager.has_value());
  const telepath::BufferNamespace hot = manager.value()->GetNamespace("hot").value();
  for (telepath::BlockId block = 0; block < 4; ++block) assert(Read(hot, kHotFile, block));

  // Without hits SIEVE evicts the oldest page.
  assert(Read(hot, kHotFile, 4));
  auto resident = Resident(*manager.value());
  assert(!resident.contains({kHotFile, 0}) && resident.contains({kHotFile, 1}));

  // The child's hit on the next-oldest page sends the hand past it.
  assert(RunChild({"hit-child", name, root.path().string(), "1"}) == 0);
  assert(Read(hot, kHotFile, 5));
  resident = Resident(*manager.value());
  assert(resident.contains({kHotFile, 1}) && !resident.contains({kHotFile, 2}));
  assert(Occupancy(*manager.value())[kHot] == 4);
}

// Child: changes a page the parent's file cache loaded, through its own
// default namespace, and writes it back; the page's owner decides which of
// this process's hooks runs.
auto HookChild(const std::string &name, const std::string &root) -> int {
  std::atomic<int> reads{0};
  int files_calls = 0;
  int default_calls = 0;
  auto opened = Open(
    name, SharedMemoryMode::kAttach, root, &reads,
    Declarations([&](const telepath::BufferTag &, std::span<const std::byte>) {
      ++files_calls;
      return telepath::Ok();
    }),
    [&](const telepath::BufferTag &, std::span<const std::byte>) {
      ++default_calls;
      return telepath::Ok();
    });
  if (!opened.has_value()) return 2;
  {
    auto handle = opened.value()->ReadBufferExclusive(kFilesFile, 0);
    if (!handle.has_value()) return 3;
    handle->mutable_data()[0] = std::byte{0x5A};
  }
  if (reads.load() != 0) return 4;
  if (!opened.value()->FlushAll().has_value()) return 5;
  if (files_calls != 1 || default_calls != 0) return 6;
  return 0;
}

void ExpectHooksFollowThePageOwnerInEveryProcess() {
  const std::string name = SegmentName("hook");
  telepath::buffer_test_support::TestRootGuard root("telepath_shared_namespace_hook");
  std::atomic<int> reads{0};
  int parent_calls = 0;
  auto manager = Open(name, SharedMemoryMode::kCreate, root.path().string(), &reads, Declarations([&](const telepath::BufferTag &, std::span<const std::byte>) {
    ++parent_calls;
    return telepath::Ok();
  }));
  assert(manager.has_value());
  assert(Read(manager.value()->GetNamespace("files").value(), kFilesFile, 0));
  assert(RunChild({"hook-child", name, root.path().string()}) == 0);
  assert(parent_calls == 0);
  auto handle = manager.value()->ReadBuffer(kFilesFile, 0);
  assert(handle.has_value() && handle->data()[0] == std::byte{0x5A} && manager.value()->ExportSnapshot().dirty_page_count == 0);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc >= 4 && std::string(argv[1]) == "quota-child") return QuotaChild(argv[2], argv[3]);
  if (argc >= 5 && std::string(argv[1]) == "hit-child") return HitChild(argv[2], argv[3], std::stoull(argv[4]));
  if (argc >= 4 && std::string(argv[1]) == "hook-child") return HookChild(argv[2], argv[3]);
  ExpectDeclarationsAreValidated();
  ExpectAttachersMustDeclareTheSameNamespaces();
  ExpectQuotasHoldAcrossProcesses();
  ExpectReplacerStateIsShared();
  ExpectHooksFollowThePageOwnerInEveryProcess();
  return 0;
}
