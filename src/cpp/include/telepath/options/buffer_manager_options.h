#ifndef TELEPATH_OPTIONS_BUFFER_MANAGER_OPTIONS_H_
#define TELEPATH_OPTIONS_BUFFER_MANAGER_OPTIONS_H_

#include <cstddef>
#include <thread>

namespace telepath {

// Runtime configuration for BufferManager construction.
struct BufferManagerOptions {
  std::size_t pool_size{0};
  std::size_t page_size{0};
  // Number of striped latches protecting the page table. Set to zero to let
  // the library derive a machine-sensitive default.
  std::size_t page_table_stripe_count{0};

  // Returns the effective stripe count after applying the default policy.
  std::size_t ResolvePageTableStripeCount() const {
    if (page_table_stripe_count != 0) {
      return page_table_stripe_count;
    }

    const std::size_t hardware_threads =
        std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const std::size_t baseline = std::max<std::size_t>(16, hardware_threads * 4);
    return std::min<std::size_t>(baseline, std::max<std::size_t>(1, pool_size));
  }
};

}  // namespace telepath

#endif  // TELEPATH_OPTIONS_BUFFER_MANAGER_OPTIONS_H_
