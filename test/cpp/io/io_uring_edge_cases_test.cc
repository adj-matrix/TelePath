#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>

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

  const fs::path root =
      fs::temp_directory_path() / "telepath_io_uring_edge_cases_test";
  fs::remove_all(root);
  fs::create_directories(root);

  telepath::IoUringDiskBackend backend(root.string(), 4096, 1);
  if (!backend.initialization_status().ok()) {
    assert(!RequireIoUringSuccess());
    assert(backend.initialization_status().code() ==
           telepath::StatusCode::kUnavailable);
    fs::remove_all(root);
    return 0;
  }

  const telepath::BufferTag zero_fill_tag{23, 7};
  std::array<std::byte, 4096> zero_fill_page{};
  zero_fill_page.fill(std::byte{0xFF});

  auto zero_fill_request =
      backend.SubmitRead(zero_fill_tag, zero_fill_page.data(),
                         zero_fill_page.size());
  assert(zero_fill_request.ok());
  auto zero_fill_completion = backend.PollCompletion();
  assert(zero_fill_completion.ok());
  assert(zero_fill_completion.value().request_id == zero_fill_request.value());
  assert(zero_fill_completion.value().operation == telepath::DiskOperation::kRead);
  assert(zero_fill_completion.value().tag == zero_fill_tag);
  assert(zero_fill_completion.value().status.ok());
  for (const std::byte value : zero_fill_page) {
    assert(value == std::byte{0});
  }

  const telepath::BufferTag first_write_tag{23, 0};
  const telepath::BufferTag second_write_tag{23, 1};
  std::array<std::byte, 4096> first_page{};
  std::array<std::byte, 4096> second_page{};
  first_page[0] = std::byte{0x11};
  first_page[4095] = std::byte{0x22};
  second_page[0] = std::byte{0x33};
  second_page[4095] = std::byte{0x44};

  auto first_write =
      backend.SubmitWrite(first_write_tag, first_page.data(), first_page.size());
  assert(first_write.ok());

  auto null_read = backend.SubmitRead(second_write_tag, nullptr, 4096);
  assert(!null_read.ok());
  assert(null_read.status().code() == telepath::StatusCode::kInvalidArgument);

  auto wrong_size_write =
      backend.SubmitWrite(second_write_tag, second_page.data(), 1024);
  assert(!wrong_size_write.ok());
  assert(wrong_size_write.status().code() ==
         telepath::StatusCode::kInvalidArgument);

  auto first_write_completion = backend.PollCompletion();
  assert(first_write_completion.ok());
  assert(first_write_completion.value().request_id == first_write.value());
  assert(first_write_completion.value().operation ==
         telepath::DiskOperation::kWrite);
  assert(first_write_completion.value().tag == first_write_tag);
  assert(first_write_completion.value().status.ok());

  auto second_write =
      backend.SubmitWrite(second_write_tag, second_page.data(), second_page.size());
  assert(second_write.ok());
  auto second_write_completion = backend.PollCompletion();
  assert(second_write_completion.ok());
  assert(second_write_completion.value().request_id == second_write.value());
  assert(second_write_completion.value().operation ==
         telepath::DiskOperation::kWrite);
  assert(second_write_completion.value().tag == second_write_tag);
  assert(second_write_completion.value().status.ok());

  telepath::PosixDiskBackend verifier(root.string(), 4096);
  std::array<std::byte, 4096> verify_first{};
  std::array<std::byte, 4096> verify_second{};
  auto verify_first_request =
      verifier.SubmitRead(first_write_tag, verify_first.data(), verify_first.size());
  auto verify_second_request = verifier.SubmitRead(second_write_tag,
                                                   verify_second.data(),
                                                   verify_second.size());
  assert(verify_first_request.ok());
  assert(verify_second_request.ok());
  auto verify_first_completion = verifier.PollCompletion();
  auto verify_second_completion = verifier.PollCompletion();
  assert(verify_first_completion.ok());
  assert(verify_second_completion.ok());
  assert(verify_first_completion.value().status.ok());
  assert(verify_second_completion.value().status.ok());
  assert(verify_first[0] == std::byte{0x11});
  assert(verify_first[4095] == std::byte{0x22});
  assert(verify_second[0] == std::byte{0x33});
  assert(verify_second[4095] == std::byte{0x44});

  backend.Shutdown();
  fs::remove_all(root);
  return 0;
}
