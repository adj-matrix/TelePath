#include <array>
#include <cassert>
#include <cstddef>

#include "telepath/io/io_uring_disk_backend.h"

int main() {
  telepath::IoUringDiskBackend backend("/tmp/telepath_io_uring_stub", 4096, 64);
  std::array<std::byte, 4096> page{};

  {
    auto result =
        backend.SubmitRead(telepath::BufferTag{1, 0}, nullptr, page.size());
    assert(!result.ok());
    assert(result.status().code() == telepath::StatusCode::kInvalidArgument);
  }

  {
    auto result =
        backend.SubmitWrite(telepath::BufferTag{1, 0}, nullptr, page.size());
    assert(!result.ok());
    assert(result.status().code() == telepath::StatusCode::kInvalidArgument);
  }

  {
    auto result =
        backend.SubmitRead(telepath::BufferTag{1, 0}, page.data(), 1024);
    assert(!result.ok());
    assert(result.status().code() == telepath::StatusCode::kInvalidArgument);
  }

  {
    auto result =
        backend.SubmitWrite(telepath::BufferTag{1, 0}, page.data(), 1024);
    assert(!result.ok());
    assert(result.status().code() == telepath::StatusCode::kInvalidArgument);
  }

  {
    auto result =
        backend.SubmitRead(telepath::BufferTag{1, 0}, page.data(), page.size());
    assert(!result.ok());
    assert(result.status().code() == telepath::StatusCode::kUnavailable);
  }

  const auto capabilities = backend.GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kIoUring);
  assert(capabilities.supports_submit_batching);
  assert(capabilities.supports_completion_batching);
  assert(capabilities.recommended_queue_depth == 64);

  backend.Shutdown();
  auto completion = backend.PollCompletion();
  assert(!completion.ok());
  assert(completion.status().code() == telepath::StatusCode::kUnavailable);
  return 0;
}
