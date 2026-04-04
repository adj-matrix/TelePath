add_executable(telepath_smoke_test
  cpp/buffer/smoke_test.cc
)

target_link_libraries(telepath_smoke_test PRIVATE telepath)

add_test(NAME telepath_smoke_test COMMAND telepath_smoke_test)

add_executable(telepath_replacer_test
  cpp/replacer/replacer_test.cc
)

target_link_libraries(telepath_replacer_test PRIVATE telepath)

add_test(NAME telepath_replacer_test COMMAND telepath_replacer_test)

add_executable(telepath_handle_test
  cpp/buffer/handle_test.cc
)

target_link_libraries(telepath_handle_test PRIVATE telepath)

add_test(NAME telepath_handle_test COMMAND telepath_handle_test)

add_executable(telepath_telemetry_test
  cpp/telemetry/telemetry_test.cc
)

target_link_libraries(telepath_telemetry_test PRIVATE telepath)

add_test(NAME telepath_telemetry_test COMMAND telepath_telemetry_test)

add_executable(telepath_memory_layout_test
  cpp/buffer/memory_layout_test.cc
)

target_link_libraries(telepath_memory_layout_test PRIVATE telepath)

add_test(NAME telepath_memory_layout_test COMMAND telepath_memory_layout_test)

add_executable(telepath_frame_snapshot_test
  cpp/buffer/frame_snapshot_test.cc
)

target_link_libraries(telepath_frame_snapshot_test PRIVATE telepath)

add_test(NAME telepath_frame_snapshot_test COMMAND telepath_frame_snapshot_test)

add_executable(telepath_options_test
  cpp/config/options_test.cc
)

target_link_libraries(telepath_options_test PRIVATE telepath)

add_test(NAME telepath_options_test COMMAND telepath_options_test)

add_executable(telepath_init_failure_test
  cpp/buffer/init_failure_test.cc
)

target_link_libraries(telepath_init_failure_test PRIVATE telepath)

add_test(NAME telepath_init_failure_test COMMAND telepath_init_failure_test)

add_executable(telepath_concurrency_test
  cpp/buffer/concurrency_test.cc
)

target_link_libraries(telepath_concurrency_test PRIVATE telepath)

add_test(NAME telepath_concurrency_test COMMAND telepath_concurrency_test)

add_executable(telepath_resource_exhaustion_test
  cpp/buffer/resource_exhaustion_test.cc
)

target_link_libraries(telepath_resource_exhaustion_test PRIVATE telepath)

add_test(NAME telepath_resource_exhaustion_test COMMAND telepath_resource_exhaustion_test)

add_executable(telepath_disk_backend_test
  cpp/io/disk_backend_test.cc
)

target_link_libraries(telepath_disk_backend_test PRIVATE telepath)

add_test(NAME telepath_disk_backend_test COMMAND telepath_disk_backend_test)

add_executable(telepath_disk_backend_factory_test
  cpp/io/disk_backend_factory_test.cc
)

target_link_libraries(telepath_disk_backend_factory_test PRIVATE telepath)

add_test(NAME telepath_disk_backend_factory_test
         COMMAND telepath_disk_backend_factory_test)

if(TELEPATH_HAS_LIBURING)
  add_executable(telepath_io_uring_backend_test
    cpp/io/io_uring_backend_test.cc
  )

  target_link_libraries(telepath_io_uring_backend_test PRIVATE telepath)

  add_test(NAME telepath_io_uring_backend_test
           COMMAND telepath_io_uring_backend_test)

  add_executable(telepath_io_uring_shutdown_wait_test
    cpp/io/io_uring_shutdown_wait_test.cc
  )

  target_link_libraries(telepath_io_uring_shutdown_wait_test PRIVATE telepath)

  add_test(NAME telepath_io_uring_shutdown_wait_test
           COMMAND telepath_io_uring_shutdown_wait_test)

  add_executable(telepath_io_uring_multi_inflight_test
    cpp/io/io_uring_multi_inflight_test.cc
  )

  target_link_libraries(telepath_io_uring_multi_inflight_test PRIVATE telepath)

  add_test(NAME telepath_io_uring_multi_inflight_test
           COMMAND telepath_io_uring_multi_inflight_test)

  add_executable(telepath_io_uring_edge_cases_test
    cpp/io/io_uring_edge_cases_test.cc
  )

  target_link_libraries(telepath_io_uring_edge_cases_test PRIVATE telepath)

  add_test(NAME telepath_io_uring_edge_cases_test
           COMMAND telepath_io_uring_edge_cases_test)

  add_executable(telepath_io_uring_shutdown_drain_test
    cpp/io/io_uring_shutdown_drain_test.cc
  )

  target_link_libraries(telepath_io_uring_shutdown_drain_test PRIVATE telepath)

  add_test(NAME telepath_io_uring_shutdown_drain_test
           COMMAND telepath_io_uring_shutdown_drain_test)

  add_executable(telepath_io_uring_failure_paths_test
    cpp/io/io_uring_failure_paths_test.cc
  )

  target_link_libraries(telepath_io_uring_failure_paths_test PRIVATE telepath)

  add_test(NAME telepath_io_uring_failure_paths_test
           COMMAND telepath_io_uring_failure_paths_test)

  set_tests_properties(
    telepath_io_uring_backend_test
    telepath_io_uring_shutdown_wait_test
    telepath_io_uring_multi_inflight_test
    telepath_io_uring_edge_cases_test
    telepath_io_uring_shutdown_drain_test
    telepath_io_uring_failure_paths_test
    PROPERTIES LABELS "native_io_uring")
else()
  add_executable(telepath_io_uring_backend_stub_test
    cpp/io/io_uring_backend_stub_test.cc
  )

  target_link_libraries(telepath_io_uring_backend_stub_test PRIVATE telepath)

  add_test(NAME telepath_io_uring_backend_stub_test
           COMMAND telepath_io_uring_backend_stub_test)
endif()

add_executable(telepath_read_zero_fill_test
  cpp/io/read_zero_fill_test.cc
)

target_link_libraries(telepath_read_zero_fill_test PRIVATE telepath)

add_test(NAME telepath_read_zero_fill_test COMMAND telepath_read_zero_fill_test)

add_executable(telepath_completion_dispatcher_idle_test
  cpp/io/completion_dispatcher_idle_test.cc
)

target_link_libraries(telepath_completion_dispatcher_idle_test PRIVATE telepath)

add_test(NAME telepath_completion_dispatcher_idle_test
         COMMAND telepath_completion_dispatcher_idle_test)

add_executable(telepath_same_page_miss_test
  cpp/buffer/same_page_miss_test.cc
)

target_link_libraries(telepath_same_page_miss_test PRIVATE telepath)

add_test(NAME telepath_same_page_miss_test COMMAND telepath_same_page_miss_test)

add_executable(telepath_same_page_miss_failure_recovery_test
  cpp/buffer/same_page_miss_failure_recovery_test.cc
)

target_link_libraries(telepath_same_page_miss_failure_recovery_test PRIVATE telepath)

add_test(NAME telepath_same_page_miss_failure_recovery_test
         COMMAND telepath_same_page_miss_failure_recovery_test)

add_executable(telepath_different_page_parallel_miss_test
  cpp/buffer/different_page_parallel_miss_test.cc
)

target_link_libraries(telepath_different_page_parallel_miss_test PRIVATE telepath)

add_test(NAME telepath_different_page_parallel_miss_test
         COMMAND telepath_different_page_parallel_miss_test)

add_executable(telepath_eviction_test
  cpp/buffer/eviction_test.cc
)

target_link_libraries(telepath_eviction_test PRIVATE telepath)

add_test(NAME telepath_eviction_test COMMAND telepath_eviction_test)

add_executable(telepath_failure_path_test
  cpp/buffer/failure_path_test.cc
)

target_link_libraries(telepath_failure_path_test PRIVATE telepath)

add_test(NAME telepath_failure_path_test COMMAND telepath_failure_path_test)

add_executable(telepath_lru_k_behavior_test
  cpp/replacer/lru_k_behavior_test.cc
)

target_link_libraries(telepath_lru_k_behavior_test PRIVATE telepath)

add_test(NAME telepath_lru_k_behavior_test COMMAND telepath_lru_k_behavior_test)

add_executable(telepath_wait_evict_interleave_test
  cpp/buffer/wait_evict_interleave_test.cc
)

target_link_libraries(telepath_wait_evict_interleave_test PRIVATE telepath)

add_test(NAME telepath_wait_evict_interleave_test COMMAND telepath_wait_evict_interleave_test)

add_executable(telepath_completion_order_test
  cpp/benchmark/completion_order_test.cc
)

target_link_libraries(telepath_completion_order_test PRIVATE telepath)

add_test(NAME telepath_completion_order_test COMMAND telepath_completion_order_test)

add_executable(telepath_flush_consistency_test
  cpp/buffer/flush_consistency_test.cc
)

target_link_libraries(telepath_flush_consistency_test PRIVATE telepath)

add_test(NAME telepath_flush_consistency_test COMMAND telepath_flush_consistency_test)

add_executable(telepath_read_lock_flush_test
  cpp/buffer/read_lock_flush_test.cc
)

target_link_libraries(telepath_read_lock_flush_test PRIVATE telepath)

add_test(NAME telepath_read_lock_flush_test COMMAND telepath_read_lock_flush_test)

add_executable(telepath_flush_all_persistence_test
  cpp/buffer/flush_all_persistence_test.cc
)

target_link_libraries(telepath_flush_all_persistence_test PRIVATE telepath)

add_test(NAME telepath_flush_all_persistence_test COMMAND telepath_flush_all_persistence_test)

add_executable(telepath_async_flush_scheduler_test
  cpp/buffer/async_flush_scheduler_test.cc
)

target_link_libraries(telepath_async_flush_scheduler_test PRIVATE telepath)

add_test(NAME telepath_async_flush_scheduler_test
         COMMAND telepath_async_flush_scheduler_test)

add_executable(telepath_benchmark_workload_test
  cpp/benchmark/benchmark_workload_test.cc
)

target_include_directories(telepath_benchmark_workload_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/benchmark)
target_link_libraries(telepath_benchmark_workload_test PRIVATE telepath)

add_test(NAME telepath_benchmark_workload_test COMMAND telepath_benchmark_workload_test)

add_executable(telepath_benchmark_json_output_test
  cpp/benchmark/benchmark_json_output_test.cc
)

target_link_libraries(telepath_benchmark_json_output_test PRIVATE telepath)

add_test(NAME telepath_benchmark_json_output_test COMMAND telepath_benchmark_json_output_test)

add_executable(telepath_benchmark
  cpp/benchmark/benchmark_main.cc
)

target_include_directories(telepath_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/benchmark)
target_link_libraries(telepath_benchmark PRIVATE telepath)
