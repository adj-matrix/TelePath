#ifndef TELEPATH_LIB_BUFFER_OBSERVATION_PUBLISHER_H_
#define TELEPATH_LIB_BUFFER_OBSERVATION_PUBLISHER_H_

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include "telepath/common/status.h"
#include "telepath/observation/observation_format.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace telepath {

// Maps the observation area and keeps it current. Every manager using the
// area writes its own process record; one of them, elected through a lock
// that dies with its process, writes the frame snapshot.
class ObservationPublisher {
 public:
  struct Sources {
    // Fills the summary, the minimum recLSN, and one record per frame.
    std::function<void(observation::ObservationSummary *, Lsn *, std::span<observation::ObservationFrameRecord>)> frames;
    std::function<TelemetrySnapshot()> counters;
  };

  static auto Open(const ObservationOptions &options, std::size_t pool_size, std::size_t page_size, bool cross_process) -> Result<std::unique_ptr<ObservationPublisher>>;

  ObservationPublisher(const ObservationPublisher &) = delete;
  ObservationPublisher &operator=(const ObservationPublisher &) = delete;
  ~ObservationPublisher();

  void Start(Sources sources);
  void Publish();
  void Stop();

 private:
  ObservationPublisher(std::string name, std::chrono::milliseconds interval, int fd, std::byte *base, std::size_t bytes);

  auto header() const -> observation::ObservationHeader &;
  auto frames() const -> std::span<observation::ObservationFrameRecord>;
  auto ClaimProcessRecord() -> Status;
  bool TryBecomePublisher();
  void Run();

  std::string name_;
  std::chrono::milliseconds interval_;
  int fd_;
  std::byte *base_;
  std::size_t bytes_;
  observation::ObservationProcessRecord *record_{nullptr};
  bool publisher_{false};
  Sources sources_;
  std::mutex publish_latch_;
  std::mutex stop_latch_;
  std::condition_variable stop_cv_;
  bool stopping_{false};
  std::thread thread_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_OBSERVATION_PUBLISHER_H_
