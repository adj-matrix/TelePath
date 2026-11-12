#ifndef TELEPATH_BUFFER_BUFFER_NAMESPACE_H_
#define TELEPATH_BUFFER_BUFFER_NAMESPACE_H_

#include <cstddef>
#include <memory>
#include <string>

#include "telepath/buffer/buffer_access_strategy.h"
#include "telepath/buffer/buffer_handle.h"
#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "telepath/options/buffer_manager_options.h"

namespace telepath {

class BufferManager;
class ReadBuffersOperation;
class Replacer;
enum class ReadBufferMode;

// Most namespaces one manager holds, the default namespace included.
inline constexpr std::size_t kMaxNamespaces = 16;

// A namespace is a policy and accounting domain inside one cache: it has its
// own replacer, frame quota, and pre-write hook, while every namespace shares
// the frames, the page table, the I/O engine, and the background threads. A
// page keeps its identity (file, block) whichever namespace reads it, so two
// namespaces reading one file share one copy; it belongs to the namespace
// that loaded it, whose replacer and quota account for it until it is
// evicted.
//
// A private manager adds namespaces at creation (BufferManager::Create()) or
// later (BufferManager::CreateNamespace()); a cross-process cache declares
// all of them at creation, identically in every process, and shares their
// frame counts and replacer state between processes.
struct NamespaceOptions {
  // Unique within the manager; "default" names the default namespace. At
  // most 63 bytes in cross-process mode.
  std::string name;
  // Tracks this namespace's frames; its capacity must cover the whole pool,
  // since frame ids are shared by all namespaces.
  std::unique_ptr<Replacer> replacer;
  // Frames no other namespace may evict from this one. The guarantees of all
  // namespaces together may not exceed the pool.
  std::size_t min_frames{0};
  // Most frames this namespace may hold; at the limit it evicts its own pages,
  // even when other frames are free. 0 means no limit. Both quotas are read
  // without a lock, so loads running at the same moment can overshoot them
  // by one frame per load beyond the first.
  std::size_t max_frames{0};
  // Runs before this namespace's pages are written back; see
  // BufferManagerOptions::pre_write_hook, which serves the default namespace.
  // In cross-process mode each process has its own, and the process writing
  // a page back runs its hook, so the hooks should agree.
  PreWriteHook pre_write_hook;
};

// Reads pages on behalf of one namespace: pages it loads belong to it. Copies
// refer to the same namespace and stay valid while the manager lives. The
// methods behave like the BufferManager methods of the same names, which act
// for the default namespace.
class BufferNamespace {
 public:
  auto id() const noexcept -> NamespaceId { return id_; }

  auto ReadBuffer(FileId file_id, BlockId block_id, BufferAccessStrategy *strategy = nullptr) -> Result<SharedBufferHandle>;
  auto ReadBufferExclusive(FileId file_id, BlockId block_id, BufferAccessStrategy *strategy = nullptr, ReadBufferMode mode = {}) -> Result<ExclusiveBufferHandle>;
  auto PrefetchBuffer(FileId file_id, BlockId block_id, std::size_t block_count = 1) -> Status;
  auto StartReadBuffers(ReadBuffersOperation &operation, FileId file_id, BlockId block_id, std::size_t block_count, BufferAccessStrategy *strategy = nullptr) -> Status;

 private:
  friend class BufferManager;

  BufferNamespace(BufferManager *manager, NamespaceId id) : manager_(manager), id_(id) {}

  BufferManager *manager_;
  NamespaceId id_;
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_NAMESPACE_H_
