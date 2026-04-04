#include <algorithm>
#include <cassert>
#include <filesystem>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/clock_replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root = fs::temp_directory_path() / "telepath_frame_snapshot_test";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = std::make_unique<telepath::ClockReplacer>(2);
  telepath::BufferManager manager(2, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  const telepath::BufferPoolSnapshot initial = manager.ExportSnapshot();
  assert(initial.pool_size == 2);
  assert(initial.page_size == 4096);
  assert(initial.frames.size() == 2);
  for (const auto &frame : initial.frames) {
    assert(frame.state == telepath::BufferFrameState::kFree);
    assert(!frame.is_valid);
    assert(frame.pin_count == 0);
    assert(!frame.is_dirty);
  }

  auto read_result = manager.ReadBuffer(3, 9);
  assert(read_result.ok());
  telepath::BufferHandle handle = std::move(read_result.value());

  auto find_loaded_frame = [](const telepath::BufferPoolSnapshot &snapshot) {
    return std::find_if(snapshot.frames.begin(), snapshot.frames.end(),
                        [](const telepath::FrameSnapshot &frame) {
                          return frame.is_valid &&
                                 frame.tag == telepath::BufferTag{3, 9};
                        });
  };

  const telepath::BufferPoolSnapshot pinned = manager.ExportSnapshot();
  const auto pinned_frame = find_loaded_frame(pinned);
  assert(pinned_frame != pinned.frames.end());
  assert(pinned_frame->state == telepath::BufferFrameState::kResident);
  assert(pinned_frame->pin_count == 1);
  assert(!pinned_frame->is_dirty);
  assert(!pinned_frame->io_in_flight);
  assert(!pinned_frame->flush_queued);
  assert(!pinned_frame->flush_in_flight);

  assert(manager.MarkBufferDirty(handle).ok());
  const telepath::BufferPoolSnapshot dirty = manager.ExportSnapshot();
  const auto dirty_frame = find_loaded_frame(dirty);
  assert(dirty_frame != dirty.frames.end());
  assert(dirty_frame->is_dirty);
  assert(dirty_frame->dirty_generation >= 1);
  assert(dirty_frame->pin_count == 1);

  handle.Reset();
  const telepath::BufferPoolSnapshot released = manager.ExportSnapshot();
  const auto released_frame = find_loaded_frame(released);
  assert(released_frame != released.frames.end());
  assert(released_frame->pin_count == 0);
  assert(released_frame->is_dirty);
  assert(released_frame->state == telepath::BufferFrameState::kResident);

  fs::remove_all(root);
  return 0;
}
