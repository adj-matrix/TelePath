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

void BufferManagerObserver::RecordReservedVictimFlush(const BufferTag &tag) const {
  RecordDiskWrite(tag);
  RecordDirtyFlush(tag);
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

}  // namespace telepath
