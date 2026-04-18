#ifndef TELEPATH_IO_IO_URING_DISK_BACKEND_TEST_PEER_H_
#define TELEPATH_IO_IO_URING_DISK_BACKEND_TEST_PEER_H_

#include <cstddef>

#include "telepath/io/io_uring_disk_backend.h"

namespace telepath {

// Exposes narrowly scoped hooks so io_uring failure paths can be tested
// deterministically without changing the production-facing backend API.
class IoUringDiskBackendTestPeer {
 public:
  // Overrides the next submit result for deterministic failure-path tests.
  static void ForceNextSubmitResult(IoUringDiskBackend &backend, int result) {
    backend.SetNextSubmitResultForTest(result);
  }

  // Overrides the next completion result for deterministic failure-path tests.
  static void ForceNextCompletionResult(
    IoUringDiskBackend &backend,
    int result
  ) {
    backend.SetNextCompletionResultForTest(result);
  }

  // Returns the number of in-flight requests currently owned by the backend.
  static auto InFlightRequestCount(const IoUringDiskBackend &backend)
    -> std::size_t {
    return backend.InFlightRequestCountForTest();
  }
};

}  // namespace telepath

#endif  // TELEPATH_IO_IO_URING_DISK_BACKEND_TEST_PEER_H_
