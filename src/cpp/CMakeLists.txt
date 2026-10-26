set(TELEPATH_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(telepath
  lib/common/status.cc
  lib/buffer/buffer_access_strategy.cc
  lib/buffer/buffer_handle.cc
  lib/buffer/frame_control.cc
  lib/buffer/buffer_manager.cc
  lib/buffer/buffer_manager_async_read.cc
  lib/buffer/buffer_manager_cleaner.cc
  lib/buffer/buffer_manager_eviction.cc
  lib/buffer/buffer_manager_flush.cc
  lib/buffer/buffer_manager_handle.cc
  lib/buffer/buffer_manager_lifecycle.cc
  lib/buffer/buffer_manager_read.cc
  lib/buffer/buffer_manager_observer.cc
  lib/buffer/page_table.cc
  lib/buffer/cleaner_controller.cc
  lib/buffer/flush_scheduler.cc
  lib/buffer/io_engine.cc
  lib/facade/ml_prefetch_buffer.cc
  lib/facade/page_cache.cc
  lib/io/disk_backend_factory.cc
  lib/io/file_descriptor_cache.cc
  lib/io/io_uring_disk_backend.cc
  lib/io/posix_disk_backend.cc
  lib/replacer/clock_replacer.cc
  lib/replacer/clock_sweep_replacer.cc
  lib/replacer/lru_k_replacer.cc
  lib/replacer/lru_replacer.cc
  lib/replacer/plan_replacer.cc
  lib/replacer/sieve_replacer.cc
  lib/replacer/two_queue_replacer.cc
  lib/sync/futex.cc
  lib/telemetry/counter_telemetry_sink.cc
  lib/telemetry/telemetry_export.cc
  lib/telemetry/shared_memory_telemetry.cc
  lib/telemetry/noop_telemetry_sink.cc
)

target_include_directories(telepath
  PUBLIC
    ${TELEPATH_INCLUDE_DIR}
)

if(TELEPATH_HAS_LIBURING)
  target_include_directories(telepath PRIVATE ${TELEPATH_LIBURING_INCLUDE_DIR})
  target_link_libraries(telepath PRIVATE ${TELEPATH_LIBURING_LIBRARY})
endif()

if(UNIX AND NOT APPLE)
  find_library(TELEPATH_RT_LIBRARY rt)
  if(TELEPATH_RT_LIBRARY)
    target_link_libraries(telepath PRIVATE ${TELEPATH_RT_LIBRARY})
  endif()
endif()

target_compile_features(telepath PUBLIC cxx_std_23)
target_compile_definitions(telepath
  PUBLIC
    TELEPATH_HAS_LIBURING=$<BOOL:${TELEPATH_HAS_LIBURING}>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(telepath PRIVATE -Werror)
endif()
