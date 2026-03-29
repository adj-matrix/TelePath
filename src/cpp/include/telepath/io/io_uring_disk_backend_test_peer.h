#ifndef TELEPATH_IO_IO_URING_DISK_BACKEND_TEST_PEER_H_
#define TELEPATH_IO_IO_URING_DISK_BACKEND_TEST_PEER_H_

#include <cstddef>

#include "telepath/io/io_uring_disk_backend.h"

namespace telepath {

// Exposes narrowly scoped hooks so io_uring failure paths can be tested
// deterministically without changing the production-facing backend API.
class IoUringDiskBackendTestPeer {
 public:
  static void ForceNextSubmitResult(IoUringDiskBackend &backend, int result) {
    backend.SetNextSubmitResultForTest(result);
  }

  static void ForceNextCompletionResult(IoUringDiskBackend &backend,
                                        int result) {
    backend.SetNextCompletionResultForTest(result);
  }

  static std::size_t InFlightRequestCount(const IoUringDiskBackend &backend) {
    return backend.InFlightRequestCountForTest();
  }
};

}  // namespace telepath

#endif  // TELEPATH_IO_IO_URING_DISK_BACKEND_TEST_PEER_H_
