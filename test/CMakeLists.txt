add_executable(telepath_smoke_test
  cpp/smoke_test.cc
)

target_link_libraries(telepath_smoke_test PRIVATE telepath)

add_test(NAME telepath_smoke_test COMMAND telepath_smoke_test)

add_executable(telepath_replacer_test
  cpp/replacer_test.cc
)

target_link_libraries(telepath_replacer_test PRIVATE telepath)

add_test(NAME telepath_replacer_test COMMAND telepath_replacer_test)

add_executable(telepath_handle_test
  cpp/handle_test.cc
)

target_link_libraries(telepath_handle_test PRIVATE telepath)

add_test(NAME telepath_handle_test COMMAND telepath_handle_test)

add_executable(telepath_telemetry_test
  cpp/telemetry_test.cc
)

target_link_libraries(telepath_telemetry_test PRIVATE telepath)

add_test(NAME telepath_telemetry_test COMMAND telepath_telemetry_test)

add_executable(telepath_memory_layout_test
  cpp/memory_layout_test.cc
)

target_link_libraries(telepath_memory_layout_test PRIVATE telepath)

add_test(NAME telepath_memory_layout_test COMMAND telepath_memory_layout_test)

add_executable(telepath_options_test
  cpp/options_test.cc
)

target_link_libraries(telepath_options_test PRIVATE telepath)

add_test(NAME telepath_options_test COMMAND telepath_options_test)

add_executable(telepath_concurrency_test
  cpp/concurrency_test.cc
)

target_link_libraries(telepath_concurrency_test PRIVATE telepath)

add_test(NAME telepath_concurrency_test COMMAND telepath_concurrency_test)

add_executable(telepath_disk_backend_test
  cpp/disk_backend_test.cc
)

target_link_libraries(telepath_disk_backend_test PRIVATE telepath)

add_test(NAME telepath_disk_backend_test COMMAND telepath_disk_backend_test)

add_executable(telepath_same_page_miss_test
  cpp/same_page_miss_test.cc
)

target_link_libraries(telepath_same_page_miss_test PRIVATE telepath)

add_test(NAME telepath_same_page_miss_test COMMAND telepath_same_page_miss_test)

add_executable(telepath_eviction_test
  cpp/eviction_test.cc
)

target_link_libraries(telepath_eviction_test PRIVATE telepath)

add_test(NAME telepath_eviction_test COMMAND telepath_eviction_test)

add_executable(telepath_failure_path_test
  cpp/failure_path_test.cc
)

target_link_libraries(telepath_failure_path_test PRIVATE telepath)

add_test(NAME telepath_failure_path_test COMMAND telepath_failure_path_test)

add_executable(telepath_lru_k_behavior_test
  cpp/lru_k_behavior_test.cc
)

target_link_libraries(telepath_lru_k_behavior_test PRIVATE telepath)

add_test(NAME telepath_lru_k_behavior_test COMMAND telepath_lru_k_behavior_test)

add_executable(telepath_wait_evict_interleave_test
  cpp/wait_evict_interleave_test.cc
)

target_link_libraries(telepath_wait_evict_interleave_test PRIVATE telepath)

add_test(NAME telepath_wait_evict_interleave_test COMMAND telepath_wait_evict_interleave_test)

add_executable(telepath_completion_order_test
  cpp/completion_order_test.cc
)

target_link_libraries(telepath_completion_order_test PRIVATE telepath)

add_test(NAME telepath_completion_order_test COMMAND telepath_completion_order_test)

add_executable(telepath_flush_consistency_test
  cpp/flush_consistency_test.cc
)

target_link_libraries(telepath_flush_consistency_test PRIVATE telepath)

add_test(NAME telepath_flush_consistency_test COMMAND telepath_flush_consistency_test)

add_executable(telepath_benchmark
  cpp/benchmark_main.cc
)

target_link_libraries(telepath_benchmark PRIVATE telepath)
