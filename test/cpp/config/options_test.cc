#include <cassert>
#include <cstddef>
#include <filesystem>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend_factory.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  telepath::BufferManagerOptions defaults{128, 4096, 0, {}};
  assert(defaults.ResolvePageTableStripeCount() >= 1);
  assert(defaults.ResolvePageTableStripeCount() <= defaults.pool_size);
  assert(defaults.disk_backend.ResolveQueueDepth() == 32);
  assert(defaults.flush_worker_count == 0);
  assert(defaults.flush_submit_batch_size == 0);
  assert(!defaults.enable_background_cleaner);
  assert(defaults.ResolveDirtyPageHighWatermark() == 96);
  assert(defaults.ResolveDirtyPageLowWatermark() == 48);

  telepath::BufferManagerOptions custom{32, 4096, 7,
                                        {telepath::DiskBackendKind::kPosix,
                                         true, 8}};
  custom.flush_worker_count = 3;
  custom.flush_submit_batch_size = 2;
  custom.enable_background_cleaner = true;
  custom.dirty_page_high_watermark = 10;
  custom.dirty_page_low_watermark = 8;
  assert(custom.ResolvePageTableStripeCount() == 7);
  assert(custom.disk_backend.ResolveQueueDepth() == 8);
  assert(custom.flush_worker_count == 3);
  assert(custom.flush_submit_batch_size == 2);
  assert(custom.ResolveDirtyPageHighWatermark() == 10);
  assert(custom.ResolveDirtyPageLowWatermark() == 8);

  const fs::path root = fs::temp_directory_path() / "telepath_options_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend_result =
      telepath::CreateDiskBackend(root.string(), 4096, custom.disk_backend);
  assert(disk_backend_result.ok());
  auto replacer = telepath::MakeClockReplacer(32);
  telepath::BufferManager manager(custom, std::move(disk_backend_result.value()),
                                  std::move(replacer), telemetry);

  assert(manager.options().page_table_stripe_count == 7);
  assert(manager.pool_size() == 32);
  assert(manager.page_size() == 4096);
  assert(manager.options().disk_backend.ResolveQueueDepth() == 8);
  assert(manager.options().flush_worker_count == 3);
  assert(manager.options().flush_submit_batch_size == 1);
  assert(manager.options().enable_background_cleaner);
  assert(manager.options().dirty_page_high_watermark == 10);
  assert(manager.options().dirty_page_low_watermark == 8);

  fs::remove_all(root);
  return 0;
}
