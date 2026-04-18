#include <algorithm>
#include <cassert>
#include <memory>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/clock_replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildManager(const std::filesystem::path &root) -> telepath::BufferManager {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = std::make_unique<telepath::ClockReplacer>(2);
  return telepath::BufferManager(2, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

auto FindLoadedFrame(const telepath::BufferPoolSnapshot &snapshot)
    -> std::vector<telepath::FrameSnapshot>::const_iterator {
  return std::find_if(
    snapshot.frames.begin(), snapshot.frames.end(),
    [](const telepath::FrameSnapshot &frame) {
      return frame.is_valid && frame.tag == telepath::BufferTag{3, 9};
    });
}

void ExpectInitialSnapshotShowsFreeFrames(
  const telepath::BufferPoolSnapshot &snapshot
) {
  assert(snapshot.pool_size == 2);
  assert(snapshot.page_size == 4096);
  assert(snapshot.frames.size() == 2);
  for (const auto &frame : snapshot.frames) {
    assert(frame.state == telepath::BufferFrameState::kFree);
    assert(!frame.is_valid);
    assert(frame.pin_count == 0);
    assert(!frame.is_dirty);
  }
}

auto PinPageForInspection(telepath::BufferManager *manager) -> telepath::BufferHandle {
  auto read_result = manager->ReadBuffer(3, 9);
  assert(read_result.ok());
  return std::move(read_result.value());
}

void ExpectPinnedSnapshotShowsResidentFrame(
  const telepath::BufferPoolSnapshot &snapshot
) {
  const auto pinned_frame = FindLoadedFrame(snapshot);
  assert(pinned_frame != snapshot.frames.end());
  assert(pinned_frame->state == telepath::BufferFrameState::kResident);
  assert(pinned_frame->pin_count == 1);
  assert(!pinned_frame->is_dirty);
  assert(!pinned_frame->io_in_flight);
  assert(!pinned_frame->flush_queued);
  assert(!pinned_frame->flush_in_flight);
}

void ExpectDirtySnapshotShowsDirtyResidentFrame(
  const telepath::BufferPoolSnapshot &snapshot
) {
  const auto dirty_frame = FindLoadedFrame(snapshot);
  assert(dirty_frame != snapshot.frames.end());
  assert(dirty_frame->is_dirty);
  assert(dirty_frame->dirty_generation >= 1);
  assert(dirty_frame->pin_count == 1);
}

void ExpectReleasedSnapshotShowsUnpinnedResidentFrame(
  const telepath::BufferPoolSnapshot &snapshot
) {
  const auto released_frame = FindLoadedFrame(snapshot);
  assert(released_frame != snapshot.frames.end());
  assert(released_frame->pin_count == 0);
  assert(released_frame->is_dirty);
  assert(released_frame->state == telepath::BufferFrameState::kResident);
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_frame_snapshot_test");
  auto manager = BuildManager(root_guard.path());

  ExpectInitialSnapshotShowsFreeFrames(manager.ExportSnapshot());

  auto handle = PinPageForInspection(&manager);
  ExpectPinnedSnapshotShowsResidentFrame(manager.ExportSnapshot());

  assert(manager.MarkBufferDirty(handle).ok());
  ExpectDirtySnapshotShowsDirtyResidentFrame(manager.ExportSnapshot());

  handle.Reset();
  ExpectReleasedSnapshotShowsUnpinnedResidentFrame(manager.ExportSnapshot());
  return 0;
}
