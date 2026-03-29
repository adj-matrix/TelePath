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
      fs::temp_directory_path() / "telepath_io_uring_shutdown_drain_test";
  fs::remove_all(root);
  fs::create_directories(root);

  std::array<std::byte, 4096> first_page{};
  std::array<std::byte, 4096> second_page{};
  first_page[0] = std::byte{0x51};
  first_page[4095] = std::byte{0x61};
  second_page[0] = std::byte{0x71};
  second_page[4095] = std::byte{0x81};

  const telepath::BufferTag first_tag{29, 0};
  const telepath::BufferTag second_tag{29, 1};

  {
    telepath::IoUringDiskBackend backend(root.string(), 4096, 8);
    if (!backend.initialization_status().ok()) {
      assert(!RequireIoUringSuccess());
      assert(backend.initialization_status().code() ==
             telepath::StatusCode::kUnavailable);
      fs::remove_all(root);
      return 0;
    }

    auto first_request =
        backend.SubmitWrite(first_tag, first_page.data(), first_page.size());
    assert(first_request.ok());

    backend.Shutdown();

    auto first_completion = backend.PollCompletion();
    assert(first_completion.ok());
    assert(first_completion.value().request_id == first_request.value());
    assert(first_completion.value().operation == telepath::DiskOperation::kWrite);
    assert(first_completion.value().tag == first_tag);
    assert(first_completion.value().status.ok());

    auto after_drain = backend.PollCompletion();
    assert(!after_drain.ok());
    assert(after_drain.status().code() == telepath::StatusCode::kUnavailable);
  }

  {
    telepath::IoUringDiskBackend backend(root.string(), 4096, 8);
    assert(backend.initialization_status().ok());

    auto second_request =
        backend.SubmitWrite(second_tag, second_page.data(), second_page.size());
    assert(second_request.ok());
  }

  telepath::PosixDiskBackend verifier(root.string(), 4096);
  std::array<std::byte, 4096> verify_first{};
  std::array<std::byte, 4096> verify_second{};
  auto verify_first_request =
      verifier.SubmitRead(first_tag, verify_first.data(), verify_first.size());
  auto verify_second_request =
      verifier.SubmitRead(second_tag, verify_second.data(), verify_second.size());
  assert(verify_first_request.ok());
  assert(verify_second_request.ok());
  auto verify_first_completion = verifier.PollCompletion();
  auto verify_second_completion = verifier.PollCompletion();
  assert(verify_first_completion.ok());
  assert(verify_second_completion.ok());
  assert(verify_first_completion.value().status.ok());
  assert(verify_second_completion.value().status.ok());
  assert(verify_first[0] == std::byte{0x51});
  assert(verify_first[4095] == std::byte{0x61});
  assert(verify_second[0] == std::byte{0x71});
  assert(verify_second[4095] == std::byte{0x81});

  fs::remove_all(root);
  return 0;
}
