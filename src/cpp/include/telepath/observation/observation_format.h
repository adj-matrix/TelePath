#ifndef TELEPATH_OBSERVATION_OBSERVATION_FORMAT_H_
#define TELEPATH_OBSERVATION_OBSERVATION_FORMAT_H_

#include <cstddef>
#include <cstdint>

// Published binary format of the observation area, a POSIX shared-memory
// object (ObservationOptions::shared_memory_name) that other processes map
// read-only. Readers in any language depend on this file only, never on the
// kernel's internal layout. All integers are little-endian; offsets are from
// the start of the object. A change to existing fields increments
// kObservationVersion; new fields only use reserved space.
//
// Layout: ObservationHeader at 0, kObservationProcessCapacity
// ObservationProcessRecord entries at ObservationHeader::processes_offset, and
// ObservationHeader::pool_size ObservationFrameRecord entries at
// ObservationHeader::frames_offset.
//
// Consistency: the header summary and the frame records form one snapshot
// guarded by ObservationHeader::sequence, and each process record is guarded
// by its own sequence. A writer makes the sequence odd, writes, and makes it
// even again. A reader copies the guarded bytes between two reads of the
// sequence and retries when the sequence was odd or changed.
namespace telepath::observation {

inline constexpr std::uint32_t kObservationMagic = 0x424F5054;  // "TPOB"
inline constexpr std::uint16_t kObservationVersion = 1;
inline constexpr std::size_t kObservationProcessCapacity = 64;

// Bits of ObservationFrameRecord::flags.
inline constexpr std::uint8_t kFrameDirty = 1U << 0;
inline constexpr std::uint8_t kFrameWritebackQueued = 1U << 1;
inline constexpr std::uint8_t kFrameWritebackActive = 1U << 2;

// Values of ObservationFrameRecord::state; same as BufferFrameState.
inline constexpr std::uint8_t kFrameFree = 0;
inline constexpr std::uint8_t kFrameLoading = 1;
inline constexpr std::uint8_t kFrameResident = 2;

// Frame counts of one snapshot.
struct ObservationSummary {
  std::uint64_t free_frames;
  std::uint64_t loading_frames;
  std::uint64_t resident_frames;
  std::uint64_t dirty_frames;
  std::uint64_t pinned_frames;
  std::uint64_t writeback_queued_frames;
  std::uint64_t writeback_active_frames;
  // The kernel's dirty-page counter, which may lag the frame flags slightly.
  std::uint64_t dirty_page_count;
};

struct ObservationHeader {
  std::uint32_t magic;
  std::uint16_t version;
  // sizeof(ObservationHeader).
  std::uint16_t header_bytes;
  // Guards `published_unix_ms`, `publisher_pid`, `summary`, and the frames.
  std::uint64_t sequence;
  // Wall-clock time of the latest snapshot; 0 before the first one.
  std::uint64_t published_unix_ms;
  // Process that wrote the latest snapshot.
  std::uint32_t publisher_pid;
  // sizeof(ObservationFrameRecord).
  std::uint32_t frame_record_bytes;
  std::uint64_t pool_size;
  std::uint64_t page_size;
  std::uint64_t frames_offset;
  std::uint64_t processes_offset;
  // sizeof(ObservationProcessRecord).
  std::uint32_t process_record_bytes;
  std::uint32_t process_capacity;
  // 1 when the cache is shared between processes (SharedMemoryOptions).
  std::uint8_t cross_process;
  std::uint8_t reserved0[7];
  ObservationSummary summary;
  // BufferManager::MinRecLsn() of the snapshot; 0 when no page has a recLSN.
  std::uint64_t min_rec_lsn;
  std::uint8_t reserved1[104];
};

// Telemetry counters of one attached process, as reported by its
// TelemetrySink; all zero with the no-op sink.
struct ObservationProcessRecord {
  // 0 marks an unused record.
  std::uint32_t pid;
  std::uint32_t reserved0;
  std::uint64_t sequence;
  std::uint64_t updated_unix_ms;
  std::uint64_t buffer_hits;
  std::uint64_t buffer_misses;
  std::uint64_t disk_reads;
  std::uint64_t disk_writes;
  std::uint64_t evictions;
  std::uint64_t dirty_flushes;
  std::uint64_t flush_tasks_scheduled;
  std::uint64_t flush_tasks_completed;
  std::uint64_t flush_failures;
  std::uint64_t cleaner_flushes_scheduled;
  std::uint64_t cleaner_flushes_finished;
  std::uint64_t cleaner_flushes_skipped;
  std::uint64_t eviction_failures;
};

struct ObservationFrameRecord {
  std::uint32_t frame_id;
  std::uint8_t state;
  std::uint8_t flags;
  std::uint16_t pins;
  // Tag of the page; meaningful only while the state is not kFrameFree.
  std::uint32_t file_id;
  // Reserved for shared namespaces; always 0.
  std::uint32_t namespace_id;
  std::uint64_t block_id;
  std::uint64_t version;
};

static_assert(sizeof(ObservationSummary) == 64);
static_assert(sizeof(ObservationHeader) == 256);
static_assert(offsetof(ObservationHeader, summary) == 80);
static_assert(offsetof(ObservationHeader, min_rec_lsn) == 144);
static_assert(sizeof(ObservationProcessRecord) == 128);
static_assert(sizeof(ObservationFrameRecord) == 32);

}  // namespace telepath::observation

#endif  // TELEPATH_OBSERVATION_OBSERVATION_FORMAT_H_
