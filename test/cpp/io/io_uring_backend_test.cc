#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "telepath/io/disk_backend_factory.h"
#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

bool RequireIoUringSuccess() {
  const char *value = std::getenv("TELEPATH_REQUIRE_IO_URING_SUCCESS");
  return value != nullptr && std::string(value) == "1";
}

}  // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path root = fs::temp_directory_path() / "telepath_io_uring_backend_test";
  fs::remove_all(root);
  fs::create_directories(root);

  telepath::IoUringDiskBackend backend(root.string(), 4096, 32);
  if (!backend.initialization_status().ok()) {
    assert(!RequireIoUringSuccess());
    assert(backend.initialization_status().code() ==
           telepath::StatusCode::kUnavailable);

    telepath::DiskBackendOptions strict_options;
    strict_options.preferred_kind = telepath::DiskBackendKind::kIoUring;
    strict_options.allow_fallback = false;
    auto strict_backend_result =
        telepath::CreateDiskBackend(root.string(), 4096, strict_options);
    assert(!strict_backend_result.ok());
    assert(strict_backend_result.status().code() ==
           telepath::StatusCode::kUnavailable);

    telepath::DiskBackendOptions fallback_options;
    fallback_options.preferred_kind = telepath::DiskBackendKind::kIoUring;
    fallback_options.allow_fallback = true;
    auto fallback_backend_result =
        telepath::CreateDiskBackend(root.string(), 4096, fallback_options);
    assert(fallback_backend_result.ok());
    const auto fallback_capabilities =
        fallback_backend_result.value()->GetCapabilities();
    assert(fallback_capabilities.kind == telepath::DiskBackendKind::kPosix);
    assert(fallback_capabilities.is_fallback_backend);
    fallback_backend_result.value()->Shutdown();

    fs::remove_all(root);
    return 0;
  }

  std::array<std::byte, 4096> write_buffer{};
  std::array<std::byte, 4096> read_buffer{};
  write_buffer[0] = std::byte{0x12};
  write_buffer[4095] = std::byte{0x34};

  const telepath::BufferTag tag{7, 3};

  auto write_request = backend.SubmitWrite(tag, write_buffer.data(),
                                           write_buffer.size());
  assert(write_request.ok());
  auto write_completion = backend.PollCompletion();
  assert(write_completion.ok());
  assert(write_completion.value().request_id == write_request.value());
  assert(write_completion.value().status.ok());

  auto read_request = backend.SubmitRead(tag, read_buffer.data(),
                                         read_buffer.size());
  assert(read_request.ok());
  auto read_completion = backend.PollCompletion();
  assert(read_completion.ok());
  assert(read_completion.value().request_id == read_request.value());
  assert(read_completion.value().status.ok());
  assert(read_buffer[0] == std::byte{0x12});
  assert(read_buffer[4095] == std::byte{0x34});

  telepath::PosixDiskBackend verifier(root.string(), 4096);
  std::array<std::byte, 4096> persisted_page{};
  auto persisted_request =
      verifier.SubmitRead(tag, persisted_page.data(), persisted_page.size());
  assert(persisted_request.ok());
  auto persisted_completion = verifier.PollCompletion();
  assert(persisted_completion.ok());
  assert(persisted_completion.value().status.ok());
  assert(persisted_page[0] == std::byte{0x12});
  assert(persisted_page[4095] == std::byte{0x34});

  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  options.allow_fallback = false;
  auto backend_result =
      telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(backend_result.ok());
  const auto capabilities = backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kIoUring);
  assert(!capabilities.is_fallback_backend);
  backend_result.value()->Shutdown();

  telepath::DiskBackendOptions auto_options;
  auto_options.preferred_kind = telepath::DiskBackendKind::kAuto;
  auto auto_backend_result =
      telepath::CreateDiskBackend(root.string(), 4096, auto_options);
  assert(auto_backend_result.ok());
  const auto auto_capabilities = auto_backend_result.value()->GetCapabilities();
  assert(auto_capabilities.kind == telepath::DiskBackendKind::kIoUring);
  assert(!auto_capabilities.is_fallback_backend);
  auto_backend_result.value()->Shutdown();

  backend.Shutdown();
  auto post_shutdown_write = backend.SubmitWrite(tag, write_buffer.data(),
                                                 write_buffer.size());
  assert(!post_shutdown_write.ok());
  assert(post_shutdown_write.status().code() ==
         telepath::StatusCode::kUnavailable);
  auto post_shutdown_completion = backend.PollCompletion();
  assert(!post_shutdown_completion.ok());
  assert(post_shutdown_completion.status().code() ==
         telepath::StatusCode::kUnavailable);

  fs::remove_all(root);
  return 0;
}
