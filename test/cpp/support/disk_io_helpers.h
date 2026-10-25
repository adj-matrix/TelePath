#ifndef TELEPATH_TEST_CPP_SUPPORT_DISK_IO_HELPERS_H_
#define TELEPATH_TEST_CPP_SUPPORT_DISK_IO_HELPERS_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "telepath/io/disk_backend.h"

namespace telepath::test_support {

inline auto NextToken() -> uint64_t {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1);
}

inline auto MakeBlockRequest(DiskOperation operation, const BufferTag &tag, std::byte *buffer, uint64_t token = 1) -> DiskRequest {
  DiskRequest request;
  request.token = token;
  request.operation = operation;
  request.tag = tag;
  request.buffers[0] = buffer;
  return request;
}

inline auto SubmitOne(DiskBackend &backend, const DiskRequest &request, DiskSubmitMode mode = DiskSubmitMode::kAsync) -> Status {
  Result<std::size_t> accepted = backend.Submit(std::span<const DiskRequest>(&request, 1), mode);
  if (!accepted.has_value()) return std::unexpected(accepted.error());
  if (accepted.value() != 1) return Internal("request was not accepted");
  return Ok();
}

// Blocks until one completion is reaped, retrying spurious wakeups.
inline auto ReapOne(DiskBackend &backend) -> Result<DiskCompletion> {
  std::array<DiskCompletion, 1> out;
  while (true) {
    Result<std::size_t> reaped = backend.Reap(out, true);
    if (!reaped.has_value()) return std::unexpected(reaped.error());
    if (reaped.value() == 1) return out[0];
  }
}

inline auto TransferBlock(DiskBackend &backend, DiskOperation operation, const BufferTag &tag, std::byte *buffer) -> Status {
  if (Status submitted = SubmitOne(backend, MakeBlockRequest(operation, tag, buffer)); !submitted.has_value()) return submitted;
  Result<DiskCompletion> completion = ReapOne(backend);
  if (!completion.has_value()) return std::unexpected(completion.error());
  return completion->status;
}

inline auto ReadBlock(DiskBackend &backend, const BufferTag &tag, std::span<std::byte> out) -> Status {
  return TransferBlock(backend, DiskOperation::kRead, tag, out.data());
}

inline auto WriteBlock(DiskBackend &backend, const BufferTag &tag, std::span<const std::byte> data) -> Status {
  return TransferBlock(backend, DiskOperation::kWrite, tag, const_cast<std::byte *>(data.data()));
}

}  // namespace telepath::test_support

#endif  // TELEPATH_TEST_CPP_SUPPORT_DISK_IO_HELPERS_H_
