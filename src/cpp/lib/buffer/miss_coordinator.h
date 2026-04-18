#ifndef TELEPATH_LIB_BUFFER_MISS_COORDINATOR_H_
#define TELEPATH_LIB_BUFFER_MISS_COORDINATOR_H_

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

struct BufferManagerMissState {
  std::mutex latch;
  std::condition_variable cv;
  bool completed{false};
  Status status{};
  FrameId frame_id{kInvalidFrameId};
};

struct BufferManagerMissRegistration {
  std::shared_ptr<BufferManagerMissState> state;
  bool is_owner{false};
};

// Coordinates same-page misses so only one thread performs the load work for a
// given BufferTag while followers wait for the owner's outcome.
class BufferManagerMissCoordinator {
 public:
  explicit BufferManagerMissCoordinator(std::size_t stripe_count);

  auto RegisterOrJoin(const BufferTag &tag) -> BufferManagerMissRegistration;
  void Complete(
    const BufferTag &tag,
    const std::shared_ptr<BufferManagerMissState> &state,
    const Status &status, FrameId frame_id
  );
  auto Wait(const std::shared_ptr<BufferManagerMissState> &state)
    -> Result<FrameId>;

 private:
  auto StripeForTag(const BufferTag &tag) const -> std::size_t;

  std::vector<std::mutex> latches_;
  std::vector<std::unordered_map<
    BufferTag,
    std::shared_ptr<BufferManagerMissState>,
    BufferTagHash
  >> state_shards_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_MISS_COORDINATOR_H_
