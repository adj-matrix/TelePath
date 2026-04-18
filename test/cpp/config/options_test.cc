#include <cassert>
#include <cstddef>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend_factory.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

class TestRootGuard {
 public:
  explicit TestRootGuard(std::string name) : root_path_(BuildRootPath(std::move(name))) {
    std::filesystem::remove_all(root_path_);
    std::filesystem::create_directories(root_path_);
  }

  ~TestRootGuard() { std::filesystem::remove_all(root_path_); }

  auto path() const -> const std::filesystem::path & { return root_path_; }

 private:
  static auto BuildRootPath(std::string name) -> std::filesystem::path {
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / (std::move(name) + "_" + std::to_string(unique_suffix));
  }

  std::filesystem::path root_path_;
};

void AssertDefaultOptions() {
  telepath::BufferManagerOptions defaults{128, 4096, 0, {}};
  assert(defaults.ResolvePageTableStripeCount() >= 1);
  assert(defaults.ResolvePageTableStripeCount() <= defaults.pool_size);
  assert(defaults.disk_backend.ResolveQueueDepth() == 32);
  assert(defaults.flush_worker_count == 0);
  assert(defaults.flush_submit_batch_size == 0);
  assert(defaults.flush_foreground_burst_limit == 0);
  assert(!defaults.enable_background_cleaner);
  assert(defaults.ResolveDirtyPageHighWatermark() == 96);
  assert(defaults.ResolveDirtyPageLowWatermark() == 48);
}

auto BuildCustomOptions() -> telepath::BufferManagerOptions {
  telepath::BufferManagerOptions custom{32, 4096, 7, {telepath::DiskBackendKind::kPosix, true, 8}};
  custom.flush_worker_count = 3;
  custom.flush_submit_batch_size = 2;
  custom.flush_foreground_burst_limit = 5;
  custom.enable_background_cleaner = true;
  custom.dirty_page_high_watermark = 10;
  custom.dirty_page_low_watermark = 8;
  return custom;
}

void AssertCustomOptions(const telepath::BufferManagerOptions &custom) {
  assert(custom.ResolvePageTableStripeCount() == 7);
  assert(custom.disk_backend.ResolveQueueDepth() == 8);
  assert(custom.flush_worker_count == 3);
  assert(custom.flush_submit_batch_size == 2);
  assert(custom.flush_foreground_burst_limit == 5);
  assert(custom.ResolveDirtyPageHighWatermark() == 10);
  assert(custom.ResolveDirtyPageLowWatermark() == 8);
}

void AssertManagerAppliesResolvedOptions(const std::filesystem::path &root, const telepath::BufferManagerOptions &custom) {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend_result = telepath::CreateDiskBackend(root.string(), 4096, custom.disk_backend);
  assert(disk_backend_result.ok());

  auto replacer = telepath::MakeClockReplacer(32);
  telepath::BufferManager manager(custom, std::move(disk_backend_result.value()), std::move(replacer), telemetry);

  assert(manager.options().page_table_stripe_count == 7);
  assert(manager.pool_size() == 32);
  assert(manager.page_size() == 4096);
  assert(manager.options().disk_backend.ResolveQueueDepth() == 8);
  assert(manager.options().flush_worker_count == 3);
  assert(manager.options().flush_submit_batch_size == 1);
  assert(manager.options().flush_foreground_burst_limit == 5);
  assert(manager.options().enable_background_cleaner);
  assert(manager.options().dirty_page_high_watermark == 10);
  assert(manager.options().dirty_page_low_watermark == 8);
}

}  // namespace

int main() {
  AssertDefaultOptions();
  auto custom = BuildCustomOptions();
  AssertCustomOptions(custom);

  TestRootGuard root_guard("telepath_options_test_data");
  AssertManagerAppliesResolvedOptions(root_guard.path(), custom);
  return 0;
}
