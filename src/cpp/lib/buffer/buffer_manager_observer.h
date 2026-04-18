#ifndef TELEPATH_LIB_BUFFER_BUFFER_MANAGER_OBSERVER_H_
#define TELEPATH_LIB_BUFFER_BUFFER_MANAGER_OBSERVER_H_

#include <memory>

#include "telepath/common/types.h"

namespace telepath {

class TelemetrySink;

class BufferManagerObserver {
 public:
  explicit BufferManagerObserver(std::shared_ptr<TelemetrySink> telemetry_sink);

  void RecordResidentHit(const BufferTag &tag) const;
  void RecordJoinedMissHit(const BufferTag &tag) const;
  void RecordReadMiss(const BufferTag &tag) const;
  void RecordReservedVictimFlush(const BufferTag &tag) const;
  void RecordLoadCompletion(const BufferTag &tag) const;
  void RecordLoadCompletion(
    const BufferTag &tag,
    const BufferTag &evicted_tag
  ) const;
  void RecordSuccessfulFlush(const BufferTag &tag) const;

 private:
  void RecordHit(const BufferTag &tag) const;
  void RecordMiss(const BufferTag &tag) const;
  void RecordDiskRead(const BufferTag &tag) const;
  void RecordDiskWrite(const BufferTag &tag) const;
  void RecordEviction(const BufferTag &tag) const;
  void RecordDirtyFlush(const BufferTag &tag) const;

  std::shared_ptr<TelemetrySink> telemetry_sink_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_BUFFER_MANAGER_OBSERVER_H_
