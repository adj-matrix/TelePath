#include <cassert>
#include <cstddef>
#include <filesystem>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  telepath::BufferManagerOptions defaults{128, 4096, 0};
  assert(defaults.ResolvePageTableStripeCount() >= 1);
  assert(defaults.ResolvePageTableStripeCount() <= defaults.pool_size);

  telepath::BufferManagerOptions custom{32, 4096, 7};
  assert(custom.ResolvePageTableStripeCount() == 7);

  const fs::path root = fs::temp_directory_path() / "telepath_options_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(32);
  telepath::BufferManager manager(custom, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  assert(manager.options().page_table_stripe_count == 7);
  assert(manager.pool_size() == 32);
  assert(manager.page_size() == 4096);

  fs::remove_all(root);
  return 0;
}
