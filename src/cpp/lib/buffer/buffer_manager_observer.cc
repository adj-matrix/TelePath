#include "buffer_manager_observer.h"

#include <utility>

#include "telepath/telemetry/telemetry_sink.h"

namespace telepath {

BufferManagerObserver::BufferManagerObserver(std::shared_ptr<TelemetrySink> telemetry_sink) : telemetry_sink_(std::move(telemetry_sink)) {
  if (telemetry_sink_ == nullptr) telemetry_sink_ = MakeNoOpTelemetrySink();
}

void BufferManagerObserver::RecordResidentHit(const BufferTag &tag) const {
  RecordHit(tag);
}

void BufferManagerObserver::RecordJoinedMissHit(const BufferTag &tag) const {
  RecordHit(tag);
}

void BufferManagerObserver::RecordReadMiss(const BufferTag &tag) const {
  RecordMiss(tag);
}

void BufferManagerObserver::RecordHit(const BufferTag &tag) const {
  telemetry_sink_->RecordHit(tag);
}

void BufferManagerObserver::RecordMiss(const BufferTag &tag) const {
  telemetry_sink_->RecordMiss(tag);
}

void BufferManagerObserver::RecordDiskRead(const BufferTag &tag) const {
  telemetry_sink_->RecordDiskRead(tag);
}

void BufferManagerObserver::RecordDiskWrite(const BufferTag &tag) const {
  telemetry_sink_->RecordDiskWrite(tag);
}

void BufferManagerObserver::RecordEviction(const BufferTag &tag) const {
  telemetry_sink_->RecordEviction(tag);
}

void BufferManagerObserver::RecordDirtyFlush(const BufferTag &tag) const {
  telemetry_sink_->RecordDirtyFlush(tag);
}

void BufferManagerObserver::RecordFlushFailure(const BufferTag &tag) const {
  telemetry_sink_->RecordFlushFailure(tag);
}

void BufferManagerObserver::RecordLoadCompletion(const BufferTag &tag) const {
  RecordDiskRead(tag);
}

void BufferManagerObserver::RecordLoadCompletion(const BufferTag &tag, const BufferTag &evicted_tag) const {
  RecordDiskRead(tag);
  RecordEviction(evicted_tag);
}

void BufferManagerObserver::RecordSuccessfulFlush(const BufferTag &tag) const {
  RecordDiskWrite(tag);
  RecordDirtyFlush(tag);
}

void BufferManagerObserver::RecordFlushTaskScheduled(const BufferTag &tag) const {
  telemetry_sink_->RecordFlushTaskScheduled(tag);
}

void BufferManagerObserver::RecordFlushTaskCompletion(const BufferTag &tag, const Status &status) const {
  telemetry_sink_->RecordFlushTaskCompleted(tag);
  if (!status.ok()) RecordFlushFailure(tag);
}

void BufferManagerObserver::RecordCleanerFlushScheduled(const BufferTag &tag) const {
  telemetry_sink_->RecordCleanerFlushScheduled(tag);
}

void BufferManagerObserver::RecordCleanerFlushFinished(const BufferTag &tag) const {
  telemetry_sink_->RecordCleanerFlushFinished(tag);
}

void BufferManagerObserver::RecordCleanerFlushSkipped() const {
  telemetry_sink_->RecordCleanerFlushSkipped();
}

void BufferManagerObserver::RecordEvictionFailure(const BufferTag &tag) const {
  telemetry_sink_->RecordEvictionFailure(tag);
}

}  // namespace telepath
