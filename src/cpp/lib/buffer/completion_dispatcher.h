#ifndef TELEPATH_LIB_BUFFER_COMPLETION_DISPATCHER_H_
#define TELEPATH_LIB_BUFFER_COMPLETION_DISPATCHER_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "telepath/common/status.h"

namespace telepath {

class DiskBackend;

// Centralizes disk-request completion ownership so request submitters can wait
// on their own request ids without racing to consume backend completions.
class BufferManagerCompletionDispatcher {
 public:
  explicit BufferManagerCompletionDispatcher(DiskBackend *disk_backend);
  ~BufferManagerCompletionDispatcher();

  auto Start() -> Status;
  void Shutdown(const Status &status);

  void Register(uint64_t request_id);
  auto Wait(uint64_t request_id) -> Status;

 private:
  struct RequestState {
    bool completed{false};
    Status status{};
  };

  void Run();

  DiskBackend *disk_backend_{nullptr};
  std::mutex latch_;
  std::condition_variable cv_;
  std::unordered_map<uint64_t, RequestState> request_states_;
  std::size_t outstanding_requests_{0};
  bool shutdown_{false};
  bool stopped_{false};
  Status shutdown_status_{Status::Unavailable("completion dispatcher stopped")};
  std::thread thread_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_COMPLETION_DISPATCHER_H_
