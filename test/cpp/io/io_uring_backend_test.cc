#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>

#include "io_test_support.h"
#include "telepath/io/disk_backend_factory.h"
#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

auto BuildMarkedPage(std::byte first, std::byte last) -> std::array<std::byte, 4096> {
  std::array<std::byte, 4096> page{};
  page[0] = first;
  page[4095] = last;
  return page;
}

void AssertPageMarkers(const std::array<std::byte, 4096> &page, std::byte first, std::byte last) {
  assert(page[0] == first);
  assert(page[4095] == last);
}

void AssertStrictIoUringCreationFails(const std::filesystem::path &root) {
  telepath::DiskBackendOptions strict_options;
  strict_options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  strict_options.allow_fallback = false;

  auto strict_backend_result = telepath::CreateDiskBackend(root.string(), 4096, strict_options);
  assert(!strict_backend_result.ok());
  assert(strict_backend_result.status().code() == telepath::StatusCode::kUnavailable);
}

void AssertIoUringCreationFallsBackToPosix(const std::filesystem::path &root) {
  telepath::DiskBackendOptions fallback_options;
  fallback_options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  fallback_options.allow_fallback = true;

  auto fallback_backend_result = telepath::CreateDiskBackend(root.string(), 4096, fallback_options);
  assert(fallback_backend_result.ok());

  const auto capabilities = fallback_backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kPosix);
  assert(capabilities.is_fallback_backend);
  fallback_backend_result.value()->Shutdown();
}

bool HandleUnavailableIoUringBackend(
    const std::filesystem::path &root, const telepath::IoUringDiskBackend &backend) {
  if (backend.initialization_status().ok()) return false;

  assert(!telepath::io_test_support::RequireIoUringSuccess());
  assert(backend.initialization_status().code() == telepath::StatusCode::kUnavailable);
  AssertStrictIoUringCreationFails(root);
  AssertIoUringCreationFallsBackToPosix(root);
  return true;
}

void AssertRoundTrip(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &write_buffer) {
  std::array<std::byte, 4096> read_buffer{};

  auto write_request = backend->SubmitWrite(tag, write_buffer.data(), write_buffer.size());
  assert(write_request.ok());

  auto write_completion = backend->PollCompletion();
  assert(write_completion.ok());
  assert(write_completion.value().request_id == write_request.value());
  assert(write_completion.value().status.ok());

  auto read_request = backend->SubmitRead(tag, read_buffer.data(), read_buffer.size());
  assert(read_request.ok());

  auto read_completion = backend->PollCompletion();
  assert(read_completion.ok());
  assert(read_completion.value().request_id == read_request.value());
  assert(read_completion.value().status.ok());
  AssertPageMarkers(read_buffer, write_buffer[0], write_buffer[4095]);
}

void AssertPersistedWriteVisibleToPosix(
    const std::filesystem::path &root,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &expected_page) {
  telepath::PosixDiskBackend verifier(root.string(), 4096);
  std::array<std::byte, 4096> persisted_page{};

  auto persisted_request = verifier.SubmitRead(tag, persisted_page.data(), persisted_page.size());
  assert(persisted_request.ok());

  auto persisted_completion = verifier.PollCompletion();
  assert(persisted_completion.ok());
  assert(persisted_completion.value().status.ok());
  AssertPageMarkers(persisted_page, expected_page[0], expected_page[4095]);
}

void AssertExplicitIoUringFactorySelection(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  options.allow_fallback = false;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(backend_result.ok());

  const auto capabilities = backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kIoUring);
  assert(!capabilities.is_fallback_backend);
  backend_result.value()->Shutdown();
}

void AssertAutoFactorySelectionPrefersIoUring(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kAuto;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(backend_result.ok());

  const auto capabilities = backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kIoUring);
  assert(!capabilities.is_fallback_backend);
  backend_result.value()->Shutdown();
}

void AssertShutdownStopsNewRequests(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  backend->Shutdown();

  auto post_shutdown_write = backend->SubmitWrite(tag, page.data(), page.size());
  assert(!post_shutdown_write.ok());
  assert(post_shutdown_write.status().code() == telepath::StatusCode::kUnavailable);

  auto post_shutdown_completion = backend->PollCompletion();
  assert(!post_shutdown_completion.ok());
  assert(post_shutdown_completion.status().code() == telepath::StatusCode::kUnavailable);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_backend_test");
  telepath::IoUringDiskBackend backend(root_guard.path().string(), 4096, 32);
  if (HandleUnavailableIoUringBackend(root_guard.path(), backend)) return 0;

  auto write_buffer = BuildMarkedPage(std::byte{0x12}, std::byte{0x34});
  const telepath::BufferTag tag{7, 3};
  AssertRoundTrip(&backend, tag, write_buffer);
  AssertPersistedWriteVisibleToPosix(root_guard.path(), tag, write_buffer);
  AssertExplicitIoUringFactorySelection(root_guard.path());
  AssertAutoFactorySelectionPrefersIoUring(root_guard.path());
  AssertShutdownStopsNewRequests(&backend, tag, write_buffer);
  return 0;
}
