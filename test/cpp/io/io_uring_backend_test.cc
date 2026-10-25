#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <utility>

#include "disk_io_helpers.h"
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
  assert(!strict_backend_result.has_value());
  assert(strict_backend_result.error().code() == telepath::ErrorCode::kUnavailable);
}

void AssertIoUringCreationFallsBackToPosix(const std::filesystem::path &root) {
  telepath::DiskBackendOptions fallback_options;
  fallback_options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  fallback_options.allow_fallback = true;

  auto fallback_backend_result = telepath::CreateDiskBackend(root.string(), 4096, fallback_options);
  assert(fallback_backend_result.has_value());

  const auto capabilities = fallback_backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kPosix);
  assert(capabilities.is_fallback_backend);
  fallback_backend_result.value()->Shutdown();
}

bool HandleUnavailableIoUringBackend(
    const std::filesystem::path &root, const telepath::Result<std::unique_ptr<telepath::IoUringDiskBackend>> &backend_result) {
  if (backend_result.has_value()) return false;

  assert(!telepath::io_test_support::RequireIoUringSuccess());
  assert(backend_result.error().code() == telepath::ErrorCode::kUnavailable);
  AssertStrictIoUringCreationFails(root);
  AssertIoUringCreationFallsBackToPosix(root);
  return true;
}

void AssertRoundTrip(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &write_buffer) {
  std::array<std::byte, 4096> read_buffer{};

  const uint64_t write_request = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(*backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, tag, const_cast<std::byte *>(write_buffer.data()), write_request)).has_value());

  auto write_completion = telepath::test_support::ReapOne(*backend);
  assert(write_completion.has_value());
  assert(write_completion.value().token == write_request);
  assert(write_completion.value().status.has_value());

  const uint64_t read_request = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(*backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag, const_cast<std::byte *>(read_buffer.data()), read_request)).has_value());

  auto read_completion = telepath::test_support::ReapOne(*backend);
  assert(read_completion.has_value());
  assert(read_completion.value().token == read_request);
  assert(read_completion.value().status.has_value());
  AssertPageMarkers(read_buffer, write_buffer[0], write_buffer[4095]);
}

void AssertPersistedWriteVisibleToPosix(
    const std::filesystem::path &root,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &expected_page) {
  telepath::PosixDiskBackend verifier(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::byte, 4096> persisted_page{};

  const uint64_t persisted_request = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(verifier, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag, const_cast<std::byte *>(persisted_page.data()), persisted_request)).has_value());

  auto persisted_completion = telepath::test_support::ReapOne(verifier);
  assert(persisted_completion.has_value());
  assert(persisted_completion.value().status.has_value());
  AssertPageMarkers(persisted_page, expected_page[0], expected_page[4095]);
}

void AssertIoUringFactoryHonorsFdCacheLimit(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  options.allow_fallback = false;
  options.max_open_files = 1;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  if (!backend_result.has_value()) {
    assert(!telepath::io_test_support::RequireIoUringSuccess());
    return;
  }

  auto backend = std::move(backend_result.value());
  const auto capabilities = backend->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kIoUring);
  assert(!capabilities.is_fallback_backend);
  auto *io_backend = static_cast<telepath::IoUringDiskBackend *>(backend.get());

  const auto first_file_path = root / "file_21.tp";
  const auto second_file_path = root / "file_22.tp";
  const telepath::BufferTag first_tag{21, 0};
  const telepath::BufferTag second_tag{22, 0};
  auto write_buffer = BuildMarkedPage(std::byte{0x51}, std::byte{0x52});

  const uint64_t first_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(*io_backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, first_tag, const_cast<std::byte *>(write_buffer.data()), first_submit)).has_value());
  auto first_completion = telepath::test_support::ReapOne(*io_backend);
  assert(first_completion.has_value());
  assert(first_completion.value().token == first_submit);
  assert(first_completion.value().status.has_value());

#if defined(__linux__)
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 1);
#endif

  const uint64_t second_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(*io_backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, second_tag, const_cast<std::byte *>(write_buffer.data()), second_submit)).has_value());
  auto second_completion = telepath::test_support::ReapOne(*io_backend);
  assert(second_completion.has_value());
  assert(second_completion.value().token == second_submit);
  assert(second_completion.value().status.has_value());

#if defined(__linux__)
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 0);
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(second_file_path) == 1);
#endif

  backend->Shutdown();
  backend.reset();

#if defined(__linux__)
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 0);
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(second_file_path) == 0);
#endif
}

void AssertExplicitIoUringFactorySelection(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  options.allow_fallback = false;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(backend_result.has_value());

  const auto capabilities = backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kIoUring);
  assert(!capabilities.is_fallback_backend);
  backend_result.value()->Shutdown();
}

void AssertAutoFactorySelectionPrefersIoUring(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kAuto;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(backend_result.has_value());

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

  auto post_shutdown_write = telepath::test_support::SubmitOne(*backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, tag, const_cast<std::byte *>(page.data())));
  assert(!post_shutdown_write.has_value());
  assert(post_shutdown_write.error().code() == telepath::ErrorCode::kUnavailable);

  auto post_shutdown_completion = telepath::test_support::ReapOne(*backend);
  assert(!post_shutdown_completion.has_value());
  assert(post_shutdown_completion.error().code() == telepath::ErrorCode::kUnavailable);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_backend_test");
  auto backend_result = telepath::IoUringDiskBackend::Create(root_guard.path().string(), telepath::BlockGeometry::Of<4096>(), 32);
  if (HandleUnavailableIoUringBackend(root_guard.path(), backend_result)) return 0;
  telepath::IoUringDiskBackend &backend = *backend_result.value();

  auto write_buffer = BuildMarkedPage(std::byte{0x12}, std::byte{0x34});
  const telepath::BufferTag tag{7, 3};
  AssertRoundTrip(&backend, tag, write_buffer);
  AssertPersistedWriteVisibleToPosix(root_guard.path(), tag, write_buffer);
  AssertIoUringFactoryHonorsFdCacheLimit(root_guard.path());
  AssertExplicitIoUringFactorySelection(root_guard.path());
  AssertAutoFactorySelectionPrefersIoUring(root_guard.path());
  AssertShutdownStopsNewRequests(&backend, tag, write_buffer);
  return 0;
}
