#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <set>

#include "disk_io_helpers.h"
#include "io_test_support.h"
#include "telepath/io/disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

auto BuildMarkedPage(std::byte first, std::byte last) -> std::array<std::byte, 4096> {
  std::array<std::byte, 4096> page{};
  page[0] = first;
  page[4095] = last;
  return page;
}

void AssertRoundTripReadWrite(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<4096>());
  auto write_buffer = BuildMarkedPage(std::byte{0x33}, std::byte{0x7A});
  std::array<std::byte, 4096> read_buffer{};
  const telepath::BufferTag tag{5, 9};

  const uint64_t write_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, tag, const_cast<std::byte *>(write_buffer.data()), write_submit)).has_value());
  auto write_completion = telepath::test_support::ReapOne(backend);
  assert(write_completion.has_value());
  assert(write_completion.value().token == write_submit);
  assert(write_completion.value().status.has_value());

  const uint64_t read_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag, const_cast<std::byte *>(read_buffer.data()), read_submit)).has_value());
  auto read_completion = telepath::test_support::ReapOne(backend);
  assert(read_completion.has_value());
  assert(read_completion.value().token == read_submit);
  assert(read_completion.value().status.has_value());

  assert(read_buffer[0] == std::byte{0x33});
  assert(read_buffer[4095] == std::byte{0x7A});
}

void AssertMalformedRequestsRejected(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::byte, 4096> read_buffer{};
  const telepath::BufferTag tag{5, 9};

  auto null_buffer = telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag, nullptr);
  auto rejected = telepath::test_support::SubmitOne(backend, null_buffer);
  assert(!rejected.has_value());
  assert(rejected.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto no_blocks = telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag, read_buffer.data());
  no_blocks.block_count = 0;
  rejected = telepath::test_support::SubmitOne(backend, no_blocks);
  assert(!rejected.has_value());
  assert(rejected.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto too_many_blocks = no_blocks;
  too_many_blocks.block_count = telepath::kMaxDiskRequestBlocks + 1;
  rejected = telepath::test_support::SubmitOne(backend, too_many_blocks);
  assert(!rejected.has_value());
  assert(rejected.error().code() == telepath::ErrorCode::kInvalidArgument);
}

void AssertWaitModeCompletesOnCallingThread(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<4096>());
  auto page = BuildMarkedPage(std::byte{0x21}, std::byte{0x22});
  const auto request = telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, {6, 0}, page.data(), 77);
  assert(telepath::test_support::SubmitOne(backend, request, telepath::DiskSubmitMode::kWait).has_value());

  std::array<telepath::DiskCompletion, 4> out;
  auto reaped = backend.Reap(out, false);
  assert(reaped.has_value() && reaped.value() == 1);
  assert(out[0].token == 77);
  assert(out[0].status.has_value());
}

void AssertMultiBlockRequestsRoundTrip(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::array<std::byte, 4096>, 3> pages{};
  for (std::size_t i = 0; i < pages.size(); ++i) pages[i] = BuildMarkedPage(static_cast<std::byte>(0x40 + i), static_cast<std::byte>(0x50 + i));
  auto write = telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, {8, 1}, pages[0].data());
  write.block_count = 3;
  write.buffers[1] = pages[1].data();
  write.buffers[2] = pages[2].data();
  assert(telepath::test_support::SubmitOne(backend, write).has_value());
  assert(telepath::test_support::ReapOne(backend).value().status.has_value());

  std::array<std::array<std::byte, 4096>, 4> read_back{};
  read_back[3].fill(std::byte{0xFF});
  auto read = telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, {8, 1}, read_back[0].data());
  read.block_count = 4;
  for (std::size_t i = 1; i < read_back.size(); ++i) read.buffers[i] = read_back[i].data();
  assert(telepath::test_support::SubmitOne(backend, read, telepath::DiskSubmitMode::kWait).has_value());
  assert(telepath::test_support::ReapOne(backend).value().status.has_value());
  for (std::size_t i = 0; i < pages.size(); ++i) assert(read_back[i] == pages[i]);
  for (const std::byte value : read_back[3]) assert(value == std::byte{0});

  std::array<std::byte, 4096> single{};
  assert(telepath::test_support::ReadBlock(backend, {8, 2}, single).has_value());
  assert(single == pages[1]);
}

void AssertShutdownDrainsPendingWrites(const std::filesystem::path &root) {
  telepath::PosixDiskBackend shutdown_backend(root.string(), telepath::BlockGeometry::Of<4096>());
  auto write_buffer = BuildMarkedPage(std::byte{0x33}, std::byte{0x7A});

  const uint64_t first_shutdown_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(shutdown_backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, telepath::BufferTag{7, 1}, const_cast<std::byte *>(write_buffer.data()), first_shutdown_submit)).has_value());
  const uint64_t second_shutdown_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(shutdown_backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, telepath::BufferTag{7, 2}, const_cast<std::byte *>(write_buffer.data()), second_shutdown_submit)).has_value());

  shutdown_backend.Shutdown();

  std::set<uint64_t> pending{first_shutdown_submit, second_shutdown_submit};
  for (int i = 0; i < 2; ++i) {
    auto completion = telepath::test_support::ReapOne(shutdown_backend);
    assert(completion.has_value());
    assert(completion.value().status.has_value());
    assert(pending.erase(completion.value().token) == 1);
  }

  auto drained_completion = telepath::test_support::ReapOne(shutdown_backend);
  assert(!drained_completion.has_value());
  assert(drained_completion.error().code() == telepath::ErrorCode::kUnavailable);
}

void AssertRepeatedAccessReusesAndClosesFileDescriptor(const std::filesystem::path &root) {
#if defined(__linux__)
  const telepath::FileId file_id = 9;
  const std::filesystem::path file_path = root / "file_9.tp";
  auto write_buffer = BuildMarkedPage(std::byte{0x44}, std::byte{0x55});
  std::array<std::byte, 4096> read_buffer{};

  assert(telepath::io_test_support::CountOpenDescriptorsForPath(file_path) == 0);
  {
    telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<4096>());
    for (telepath::BlockId block_id = 0; block_id < 4; ++block_id) {
      const telepath::BufferTag tag{file_id, block_id};
      const uint64_t write_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, tag, const_cast<std::byte *>(write_buffer.data()), write_submit)).has_value());
      auto write_completion = telepath::test_support::ReapOne(backend);
      assert(write_completion.has_value());
      assert(write_completion.value().status.has_value());

      const uint64_t read_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag, const_cast<std::byte *>(read_buffer.data()), read_submit)).has_value());
      auto read_completion = telepath::test_support::ReapOne(backend);
      assert(read_completion.has_value());
      assert(read_completion.value().status.has_value());
    }

    assert(telepath::io_test_support::CountOpenDescriptorsForPath(file_path) == 1);
  }
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(file_path) == 0);
#else
  (void)root;
#endif
}

void AssertFileDescriptorCacheEvictsLeastRecentlyUsedFile(const std::filesystem::path &root) {
#if defined(__linux__)
  const telepath::FileId first_file_id = 10;
  const telepath::FileId second_file_id = 11;
  const std::filesystem::path first_file_path = root / "file_10.tp";
  const std::filesystem::path second_file_path = root / "file_11.tp";
  auto write_buffer = BuildMarkedPage(std::byte{0x66}, std::byte{0x77});

  {
    telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<4096>(), false, 1);
    const uint64_t first_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, {first_file_id, 0}, const_cast<std::byte *>(write_buffer.data()), first_submit)).has_value());
    auto first_completion = telepath::test_support::ReapOne(backend);
    assert(first_completion.has_value());
    assert(first_completion.value().status.has_value());
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 1);

    const uint64_t second_submit = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, {second_file_id, 0}, const_cast<std::byte *>(write_buffer.data()), second_submit)).has_value());
    auto second_completion = telepath::test_support::ReapOne(backend);
    assert(second_completion.has_value());
    assert(second_completion.value().status.has_value());
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 0);
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(second_file_path) == 1);

    const uint64_t first_reopen = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, {first_file_id, 1}, const_cast<std::byte *>(write_buffer.data()), first_reopen)).has_value());
    auto first_reopen_completion = telepath::test_support::ReapOne(backend);
    assert(first_reopen_completion.has_value());
    assert(first_reopen_completion.value().status.has_value());
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 1);
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(second_file_path) == 0);
  }
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 0);
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(second_file_path) == 0);
#else
  (void)root;
#endif
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_disk_backend_data");
  AssertRoundTripReadWrite(root_guard.path());
  AssertMalformedRequestsRejected(root_guard.path());
  AssertWaitModeCompletesOnCallingThread(root_guard.path());
  AssertMultiBlockRequestsRoundTrip(root_guard.path());
  AssertShutdownDrainsPendingWrites(root_guard.path());
  AssertRepeatedAccessReusesAndClosesFileDescriptor(root_guard.path());
  AssertFileDescriptorCacheEvictsLeastRecentlyUsedFile(root_guard.path());
  return 0;
}
