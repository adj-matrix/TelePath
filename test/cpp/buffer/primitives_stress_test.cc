// Concurrency stress of the kernel's building blocks, which every other test
// only exercises indirectly: the lock-free free list, the process-shared futex
// mutex, the page table's lock-free lookups, and the I/O engine's completion
// routing.
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "frame_free_list.h"
#include "io_engine.h"
#include "page_table.h"
#include "../sync/futex.h"
#include "telepath/io/disk_backend.h"

extern char **environ;

namespace {

using telepath::FrameId;

void Fail(const char *what) {
  std::fprintf(stderr, "%s\n", what);
  std::abort();
}

// Every frame is owned by exactly one thread or by the list. Tiny capacities
// with many threads recycle the same few frames constantly, which is where an
// ABA problem in the versioned head would show.
void ExpectFreeListHandsEachFrameToOneOwner() {
  for (std::size_t capacity : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{64}}) {
    telepath::StateLayout layout;
    const auto offsets = telepath::FrameFreeList::Reserve(&layout, capacity);
    telepath::StateStorage storage(layout.size());
    telepath::FrameFreeList list(capacity, storage.data(), offsets, true);
    for (FrameId frame = 0; frame < capacity; ++frame) list.Push(frame);

    std::vector<std::atomic<int>> owner(capacity);
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread) {
      threads.emplace_back([&, thread] {
        std::mt19937 rng(thread);
        std::vector<FrameId> held;
        for (int round = 0; round < 20000; ++round) {
          if (held.empty() || (held.size() < 3 && rng() % 2 == 0)) {
            const std::optional<FrameId> frame = list.Pop();
            if (!frame) continue;
            if (*frame >= capacity) Fail("free list returned an out-of-range frame");
            int expected = 0;
            if (!owner[*frame].compare_exchange_strong(expected, thread + 1)) Fail("free list handed out a frame twice");
            held.push_back(*frame);
          } else {
            const FrameId frame = held.back();
            held.pop_back();
            owner[frame].store(0);
            list.Push(frame);
          }
        }
        for (FrameId frame : held) {
          owner[frame].store(0);
          list.Push(frame);
        }
      });
    }
    for (auto &thread : threads) thread.join();
    std::vector<bool> seen(capacity, false);
    for (std::size_t count = 0; count < capacity; ++count) {
      const std::optional<FrameId> frame = list.Pop();
      if (!frame || seen[*frame]) Fail("free list lost or duplicated a frame");
      seen[*frame] = true;
    }
    if (list.Pop().has_value()) Fail("free list holds more frames than exist");
  }
}

struct Counter {
  telepath::sync::FutexMutex latch;
  std::uint64_t value{0};
};

void Increment(Counter *counter, int times) {
  for (int i = 0; i < times; ++i) {
    std::lock_guard<telepath::sync::FutexMutex> guard(counter->latch);
    ++counter->value;
  }
}

constexpr int kIncrements = 100000;

// Child of ExpectFutexMutexExcludesAcrossProcesses.
auto MutexChild(const std::string &name) -> int {
  const int fd = shm_open(name.c_str(), O_RDWR, 0);
  if (fd < 0) return 2;
  auto *counter = static_cast<Counter *>(mmap(nullptr, sizeof(Counter), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  close(fd);
  std::thread helper(Increment, counter, kIncrements);
  Increment(counter, kIncrements);
  helper.join();
  return 0;
}

// The mutex protects a plain counter for threads of two processes; a lost
// increment means two holders at once.
void ExpectFutexMutexExcludesAcrossProcesses() {
  Counter local;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 8; ++thread) threads.emplace_back(Increment, &local, kIncrements);
  for (auto &thread : threads) thread.join();
  if (local.value != 8ULL * kIncrements) Fail("futex mutex lost an increment between threads");
  assert(local.latch.try_lock());
  assert(!local.latch.try_lock());
  local.latch.unlock();

  const std::string name = "/telepath_futex_mutex_" + std::to_string(getpid());
  const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  assert(fd >= 0 && ftruncate(fd, sizeof(Counter)) == 0);
  auto *shared = static_cast<Counter *>(mmap(nullptr, sizeof(Counter), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  close(fd);
  std::construct_at(shared);
  std::vector<char *> argv{const_cast<char *>("/proc/self/exe"), const_cast<char *>("mutex-child"), const_cast<char *>(name.c_str()), nullptr};
  pid_t child = 0;
  assert(posix_spawn(&child, "/proc/self/exe", nullptr, nullptr, argv.data(), environ) == 0);
  std::thread helper(Increment, shared, kIncrements);
  Increment(shared, kIncrements);
  helper.join();
  int status = 0;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  if (shared->value != 4ULL * kIncrements) Fail("futex mutex lost an increment between processes");
  munmap(shared, sizeof(Counter));
  shm_unlink(name.c_str());
}

// Writers move their own frames between unique tags while readers look the
// tags up. A frame's tag is published with a sequence count, so a reader that
// saw the same even count before and after its lookup knows the mapping was
// stable throughout and the lookup must have found exactly that frame.
void ExpectLookupsSeeStableMappings() {
  constexpr std::size_t kFrames = 64;
  for (std::size_t stripes : {std::size_t{1}, std::size_t{4}}) {
    telepath::BufferManagerPageTable table(kFrames, stripes);
    struct Published {
      std::atomic<std::uint64_t> sequence{0};
      std::atomic<telepath::BlockId> block{0};
    };
    std::vector<Published> published(kFrames);
    for (FrameId frame = 0; frame < kFrames; ++frame) table.Install({frame, 0}, frame);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> stable_lookups{0};
    std::vector<std::thread> threads;
    for (int writer = 0; writer < 4; ++writer) {
      threads.emplace_back([&, writer] {
        std::mt19937 rng(writer);
        for (int round = 0; round < 30000; ++round) {
          const FrameId frame = static_cast<FrameId>(writer + 4 * (rng() % (kFrames / 4)));
          Published &entry = published[frame];
          const telepath::BlockId old_block = entry.block.load();
          entry.sequence.fetch_add(1);
          const telepath::BufferTag old_tag{frame, old_block};
          const telepath::BufferTag new_tag{frame, old_block + 1};
          switch (rng() % 3) {
            case 0:
              table.Replace(old_tag, new_tag, frame);
              break;
            case 1:
              table.Remove(old_tag, frame);
              table.Install(new_tag, frame);
              break;
            default: {
              auto live = table.ReplaceUnlessLive(&old_tag, new_tag, frame, [](FrameId) { return true; });
              if (live.has_value()) Fail("a unique tag was already mapped");
            }
          }
          entry.block.store(old_block + 1);
          entry.sequence.fetch_add(1);
        }
      });
    }
    for (int reader = 0; reader < 4; ++reader) {
      threads.emplace_back([&, reader] {
        std::mt19937 rng(100 + reader);
        while (!stop.load()) {
          const FrameId frame = static_cast<FrameId>(rng() % kFrames);
          const Published &entry = published[frame];
          const std::uint64_t before = entry.sequence.load();
          const telepath::BlockId block = entry.block.load();
          const std::optional<FrameId> found = table.LookupFrameId({frame, block});
          const std::optional<FrameId> stale = block == 0 ? std::nullopt : table.LookupFrameId({frame, block - 1});
          if (before % 2 != 0 || entry.sequence.load() != before) continue;
          if (found != frame) Fail("lookup missed a stable mapping");
          if (stale.has_value()) Fail("lookup found a tag that was replaced");
          stable_lookups.fetch_add(1);
        }
      });
    }
    for (int writer = 0; writer < 4; ++writer) threads[writer].join();
    stop.store(true);
    for (std::size_t reader = 4; reader < threads.size(); ++reader) threads[reader].join();
    if (stable_lookups.load() < 1000) Fail("too few stable lookups to mean anything");
  }
}

// Completes requests out of order from its own threads, some immediately
// (visible to non-blocking reaps), some failed, and accepts or rejects
// submissions partly or wholly at random.
class ChaoticBackend : public telepath::DiskBackend {
 public:
  ChaoticBackend() {
    for (int worker = 0; worker < 3; ++worker) workers_.emplace_back([this, worker] { Work(worker); });
  }
  ~ChaoticBackend() override {
    Shutdown();
    for (auto &worker : workers_) worker.join();
  }

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode) -> telepath::Result<std::size_t> override {
    std::lock_guard<std::mutex> guard(latch_);
    if (shutdown_) return telepath::Unavailable("backend is shut down");
    const unsigned roll = rng_() % 100;
    if (roll < 5) return telepath::IoError("submit rejected", 11);
    const std::size_t accepted = roll < 20 ? 1 + rng_() % requests.size() : requests.size();
    for (std::size_t i = 0; i < accepted; ++i) {
      const bool fail = rng_() % 10 == 0;
      telepath::DiskCompletion completion{requests[i].token, fail ? telepath::IoError("request failed", 5) : telepath::Ok()};
      if (rng_() % 3 == 0) ready_.push_back(completion);
      else pending_.push_back(completion);
    }
    cv_.notify_all();
    return accepted;
  }

  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override {
    std::unique_lock<std::mutex> lock(latch_);
    if (wait) cv_.wait(lock, [&] { return !ready_.empty() || (shutdown_ && pending_.empty()); });
    std::size_t count = 0;
    while (count < out.size() && !ready_.empty()) {
      out[count++] = ready_.front();
      ready_.pop_front();
    }
    if (count == 0 && wait) return telepath::Unavailable("backend is shut down");
    return count;
  }

  void Shutdown() override {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
    cv_.notify_all();
  }

  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return {}; }

 private:
  void Work(int worker) {
    std::mt19937 rng(worker);
    std::unique_lock<std::mutex> lock(latch_);
    while (true) {
      cv_.wait(lock, [&] { return !pending_.empty() || shutdown_; });
      if (pending_.empty()) return;
      const std::size_t pick = rng() % pending_.size();
      telepath::DiskCompletion completion = pending_[pick];
      pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(pick));
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
      lock.lock();
      ready_.push_back(completion);
      cv_.notify_all();
    }
  }

  std::mutex latch_;
  std::condition_variable cv_;
  std::mt19937 rng_{42};
  std::deque<telepath::DiskCompletion> pending_;
  std::deque<telepath::DiskCompletion> ready_;
  bool shutdown_{false};
  std::vector<std::thread> workers_;
};

// Forwards one completion to a batch's waiter and counts its own deliveries.
class Tracker final : public telepath::IoCompletion {
 public:
  void Bind(telepath::IoWaiter *waiter) { waiter_ = waiter; }
  void Complete(const telepath::Status &status) override {
    calls_.fetch_add(1);
    failed_.store(!status.has_value());
    waiter_->Complete(status);
  }
  auto calls() const -> int { return calls_.load(); }
  bool failed() const { return failed_.load(); }

 private:
  telepath::IoWaiter *waiter_{nullptr};
  std::atomic<int> calls_{0};
  std::atomic<bool> failed_{false};
};

// Every request completes exactly once on whichever thread reaps it, and a
// batch's waiter reports an error exactly when one of its requests failed.
void ExpectEveryCompletionDeliveredOnce() {
  ChaoticBackend backend;
  telepath::IoEngine engine(&backend);
  assert(engine.Start().has_value());
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 8; ++thread) {
    threads.emplace_back([&, thread] {
      std::mt19937 rng(thread);
      for (int batch = 0; batch < 3000; ++batch) {
        const std::size_t size = 1 + rng() % 6;
        std::vector<Tracker> trackers(size);
        telepath::IoWaiter waiter(static_cast<std::uint32_t>(size));
        std::vector<telepath::DiskRequest> requests(size);
        for (std::size_t i = 0; i < size; ++i) {
          trackers[i].Bind(&waiter);
          requests[i].token = reinterpret_cast<std::uint64_t>(static_cast<telepath::IoCompletion *>(&trackers[i]));
        }
        engine.Submit(requests, rng() % 2 == 0 ? telepath::DiskSubmitMode::kWait : telepath::DiskSubmitMode::kAsync);
        const telepath::Status status = engine.Wait(waiter);
        bool any_failed = false;
        for (const Tracker &tracker : trackers) {
          if (tracker.calls() != 1) Fail("a request completed other than exactly once");
          any_failed = any_failed || tracker.failed();
        }
        if (any_failed == status.has_value()) Fail("waiter status disagrees with its requests");
      }
    });
  }
  for (auto &thread : threads) thread.join();
  engine.Shutdown(telepath::Unavailable("test done"));
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::strcmp(argv[1], "mutex-child") == 0) return MutexChild(argv[2]);
  ExpectFreeListHandsEachFrameToOneOwner();
  ExpectFutexMutexExcludesAcrossProcesses();
  ExpectLookupsSeeStableMappings();
  ExpectEveryCompletionDeliveredOnce();
  return 0;
}
