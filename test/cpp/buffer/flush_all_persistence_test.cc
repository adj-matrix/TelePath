#include <array>
#include <cassert>
#include <filesystem>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root =
      fs::temp_directory_path() / "telepath_flush_all_persistence_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  {
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    auto disk_backend =
        std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
    auto replacer = telepath::MakeLruKReplacer(3, 2);
    telepath::BufferManager manager(3, 4096, std::move(disk_backend),
                                    std::move(replacer), telemetry);

    for (telepath::BlockId block_id = 0; block_id < 3; ++block_id) {
      auto result = manager.ReadBuffer(8, block_id);
      assert(result.ok());
      telepath::BufferHandle handle = std::move(result.value());
      handle.mutable_data()[0] = static_cast<std::byte>(0x40 + block_id);
      handle.mutable_data()[1] = static_cast<std::byte>(0x50 + block_id);
      assert(manager.MarkBufferDirty(handle).ok());
    }

    const telepath::Status flush_status = manager.FlushAll();
    assert(flush_status.ok());
  }

  auto verifier =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  for (telepath::BlockId block_id = 0; block_id < 3; ++block_id) {
    std::array<std::byte, 4096> page{};
    auto request = verifier->SubmitRead(telepath::BufferTag{8, block_id},
                                        page.data(), page.size());
    assert(request.ok());
    auto completion = verifier->PollCompletion();
    assert(completion.ok());
    assert(completion.value().status.ok());
    assert(page[0] == static_cast<std::byte>(0x40 + block_id));
    assert(page[1] == static_cast<std::byte>(0x50 + block_id));
  }

  fs::remove_all(root);
  return 0;
}
