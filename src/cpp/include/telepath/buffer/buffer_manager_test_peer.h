#ifndef TELEPATH_BUFFER_BUFFER_MANAGER_TEST_PEER_H_
#define TELEPATH_BUFFER_BUFFER_MANAGER_TEST_PEER_H_

#include <memory>

#include "telepath/buffer/buffer_handle.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

// Exposes narrowly scoped hooks so buffer-manager race paths can be tested
// deterministically without changing the production-facing API.
class BufferManagerTestPeer {
 public:
  static auto AwaitJoinedMiss(
    BufferManager &manager,
    const BufferTag &tag,
    const std::shared_ptr<BufferManagerMissState> &state
  ) -> Result<BufferHandle> {
    return manager.AwaitJoinedMiss(tag, state);
  }
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_MANAGER_TEST_PEER_H_
