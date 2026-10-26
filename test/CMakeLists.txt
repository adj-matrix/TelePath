# Tests rely on assert() side effects; keep them active in every build type.
add_compile_options(-UNDEBUG)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/cpp/support)

add_executable(telepath_smoke_test
  cpp/buffer/smoke_test.cc
)

target_link_libraries(telepath_smoke_test PRIVATE telepath)

add_test(NAME telepath_smoke_test COMMAND telepath_smoke_test)

add_executable(telepath_block_geometry_test
  cpp/common/block_geometry_test.cc
)

target_link_libraries(telepath_block_geometry_test PRIVATE telepath)

add_test(NAME telepath_block_geometry_test COMMAND telepath_block_geometry_test)

add_executable(telepath_replacer_test
  cpp/replacer/replacer_test.cc
)

target_link_libraries(telepath_replacer_test PRIVATE telepath)

add_test(NAME telepath_replacer_test COMMAND telepath_replacer_test)

add_executable(telepath_clock_family_behavior_test
  cpp/replacer/clock_family_behavior_test.cc
)

target_link_libraries(telepath_clock_family_behavior_test PRIVATE telepath)

add_test(NAME telepath_clock_family_behavior_test COMMAND telepath_clock_family_behavior_test)

add_executable(telepath_replacer_concurrency_test
  cpp/replacer/replacer_concurrency_test.cc
)

target_link_libraries(telepath_replacer_concurrency_test PRIVATE telepath)

add_test(NAME telepath_replacer_concurrency_test COMMAND telepath_replacer_concurrency_test)

add_executable(telepath_handle_test
  cpp/buffer/handle_test.cc
)

target_link_libraries(telepath_handle_test PRIVATE telepath)

add_test(NAME telepath_handle_test COMMAND telepath_handle_test)

add_executable(telepath_page_table_test
  cpp/buffer/page_table_test.cc
)

target_include_directories(telepath_page_table_test PRIVATE ${PROJECT_SOURCE_DIR}/src/cpp/lib/buffer)
target_link_libraries(telepath_page_table_test PRIVATE telepath)

add_test(NAME telepath_page_table_test COMMAND telepath_page_table_test)

add_executable(telepath_primitives_stress_test
  cpp/buffer/primitives_stress_test.cc
)

target_include_directories(telepath_primitives_stress_test PRIVATE ${PROJECT_SOURCE_DIR}/src/cpp/lib/buffer)
target_link_libraries(telepath_primitives_stress_test PRIVATE telepath)

add_test(NAME telepath_primitives_stress_test COMMAND telepath_primitives_stress_test)

add_executable(telepath_frame_control_test
  cpp/buffer/frame_control_test.cc
)

target_include_directories(telepath_frame_control_test PRIVATE ${PROJECT_SOURCE_DIR}/src/cpp/lib/buffer)
target_link_libraries(telepath_frame_control_test PRIVATE telepath)

add_test(NAME telepath_frame_control_test COMMAND telepath_frame_control_test)

add_executable(telepath_io_engine_test
  cpp/buffer/io_engine_test.cc
)

target_include_directories(telepath_io_engine_test PRIVATE ${PROJECT_SOURCE_DIR}/src/cpp/lib/buffer)
target_link_libraries(telepath_io_engine_test PRIVATE telepath)

add_test(NAME telepath_io_engine_test COMMAND telepath_io_engine_test)

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

add_executable(telepath_dirty_eviction_failure_recovery_test
  cpp/buffer/dirty_eviction_failure_recovery_test.cc
)

target_link_libraries(telepath_dirty_eviction_failure_recovery_test PRIVATE telepath)

add_test(NAME telepath_dirty_eviction_failure_recovery_test
         COMMAND telepath_dirty_eviction_failure_recovery_test)

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

add_executable(telepath_two_queue_behavior_test
  cpp/replacer/two_queue_behavior_test.cc
)

target_link_libraries(telepath_two_queue_behavior_test PRIVATE telepath)

add_test(NAME telepath_two_queue_behavior_test
         COMMAND telepath_two_queue_behavior_test)

add_executable(telepath_wait_evict_interleave_test
  cpp/buffer/wait_evict_interleave_test.cc
)

target_link_libraries(telepath_wait_evict_interleave_test PRIVATE telepath)

add_test(NAME telepath_wait_evict_interleave_test COMMAND telepath_wait_evict_interleave_test)

add_executable(telepath_fatal_state_test
  cpp/buffer/fatal_state_test.cc
)

target_link_libraries(telepath_fatal_state_test PRIVATE telepath)

add_test(NAME telepath_fatal_state_test COMMAND telepath_fatal_state_test)

add_executable(telepath_stress_integrity_test
  cpp/buffer/stress_integrity_test.cc
)

target_link_libraries(telepath_stress_integrity_test PRIVATE telepath)

add_test(NAME telepath_stress_integrity_test COMMAND telepath_stress_integrity_test)

add_executable(telepath_chaos_test
  cpp/buffer/chaos_test.cc
)

target_link_libraries(telepath_chaos_test PRIVATE telepath)

add_test(NAME telepath_chaos_test COMMAND telepath_chaos_test)

add_executable(telepath_lifecycle_stress_test
  cpp/buffer/lifecycle_stress_test.cc
)

target_link_libraries(telepath_lifecycle_stress_test PRIVATE telepath)

add_test(NAME telepath_lifecycle_stress_test COMMAND telepath_lifecycle_stress_test)
set_tests_properties(telepath_stress_integrity_test PROPERTIES TIMEOUT 300)

add_executable(telepath_buffer_access_strategy_test
  cpp/buffer/buffer_access_strategy_test.cc
)

target_link_libraries(telepath_buffer_access_strategy_test PRIVATE telepath)

add_test(NAME telepath_buffer_access_strategy_test COMMAND telepath_buffer_access_strategy_test)

add_executable(telepath_indexed_ordered_set_test
  cpp/replacer/indexed_ordered_set_test.cc
)

target_include_directories(telepath_indexed_ordered_set_test PRIVATE ${PROJECT_SOURCE_DIR}/src/cpp/lib/replacer)
target_link_libraries(telepath_indexed_ordered_set_test PRIVATE telepath)

add_test(NAME telepath_indexed_ordered_set_test COMMAND telepath_indexed_ordered_set_test)

add_executable(telepath_replacer_shared_state_test
  cpp/replacer/replacer_shared_state_test.cc
)

target_link_libraries(telepath_replacer_shared_state_test PRIVATE telepath)

add_test(NAME telepath_replacer_shared_state_test COMMAND telepath_replacer_shared_state_test)

add_executable(telepath_replacer_model_test
  cpp/replacer/replacer_model_test.cc
)

target_link_libraries(telepath_replacer_model_test PRIVATE telepath)

add_test(NAME telepath_replacer_model_test COMMAND telepath_replacer_model_test)

add_executable(telepath_plan_replacer_test
  cpp/replacer/plan_replacer_test.cc
)

target_link_libraries(telepath_plan_replacer_test PRIVATE telepath)

add_test(NAME telepath_plan_replacer_test COMMAND telepath_plan_replacer_test)

add_executable(telepath_ml_prefetch_buffer_test
  cpp/facade/ml_prefetch_buffer_test.cc
)

target_include_directories(telepath_ml_prefetch_buffer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/buffer)
target_link_libraries(telepath_ml_prefetch_buffer_test PRIVATE telepath)

add_test(NAME telepath_ml_prefetch_buffer_test COMMAND telepath_ml_prefetch_buffer_test)

add_executable(telepath_page_cache_test
  cpp/facade/page_cache_test.cc
)

target_include_directories(telepath_page_cache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/buffer)
target_link_libraries(telepath_page_cache_test PRIVATE telepath)

add_test(NAME telepath_page_cache_test COMMAND telepath_page_cache_test)

add_executable(telepath_page_cache_model_test
  cpp/facade/page_cache_model_test.cc
)

target_include_directories(telepath_page_cache_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/buffer)
target_link_libraries(telepath_page_cache_model_test PRIVATE telepath)

add_test(NAME telepath_page_cache_model_test COMMAND telepath_page_cache_model_test)

add_executable(telepath_foreground_writeback_test
  cpp/buffer/foreground_writeback_test.cc
)

target_link_libraries(telepath_foreground_writeback_test PRIVATE telepath)

add_test(NAME telepath_foreground_writeback_test COMMAND telepath_foreground_writeback_test)

add_executable(telepath_durability_test
  cpp/buffer/durability_test.cc
)

target_link_libraries(telepath_durability_test PRIVATE telepath)

add_test(NAME telepath_durability_test COMMAND telepath_durability_test)

add_executable(telepath_rec_lsn_test
  cpp/buffer/rec_lsn_test.cc
)

target_link_libraries(telepath_rec_lsn_test PRIVATE telepath)

add_test(NAME telepath_rec_lsn_test COMMAND telepath_rec_lsn_test)

add_executable(telepath_rec_lsn_stress_test
  cpp/buffer/rec_lsn_stress_test.cc
)

target_link_libraries(telepath_rec_lsn_stress_test PRIVATE telepath)

add_test(NAME telepath_rec_lsn_stress_test COMMAND telepath_rec_lsn_stress_test)

add_executable(telepath_shared_memory_test
  cpp/buffer/shared_memory_test.cc
)

target_link_libraries(telepath_shared_memory_test PRIVATE telepath)

add_test(NAME telepath_shared_memory_test COMMAND telepath_shared_memory_test)

add_executable(telepath_shared_memory_stress_test
  cpp/buffer/shared_memory_stress_test.cc
)

target_link_libraries(telepath_shared_memory_stress_test PRIVATE telepath)

add_test(NAME telepath_shared_memory_stress_test COMMAND telepath_shared_memory_stress_test)

add_executable(telepath_async_read_test
  cpp/buffer/async_read_test.cc
)

target_link_libraries(telepath_async_read_test PRIVATE telepath)

add_test(NAME telepath_async_read_test COMMAND telepath_async_read_test)
set_tests_properties(telepath_async_read_test PROPERTIES TIMEOUT 120)

add_executable(telepath_eviction_race_test
  cpp/buffer/eviction_race_test.cc
)

target_link_libraries(telepath_eviction_race_test PRIVATE telepath)

add_test(NAME telepath_eviction_race_test COMMAND telepath_eviction_race_test)
set_tests_properties(telepath_eviction_race_test PROPERTIES TIMEOUT 60)

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

find_program(TELEPATH_PYTHON3 python3)
if(TELEPATH_PYTHON3)
  add_test(NAME telepath_benchmark_summarize_test
           COMMAND ${TELEPATH_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/python/benchmark_summarize_test.py)
  add_test(NAME telepath_benchmark_workflow_test
           COMMAND ${TELEPATH_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/python/benchmark_workflow_test.py)
  add_test(NAME telepath_web_backend_parser_test
           COMMAND ${TELEPATH_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/python/web_backend_parser_test.py)
  add_test(NAME telepath_web_frontend_contract_test
           COMMAND ${TELEPATH_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/python/web_frontend_contract_test.py)
  add_test(NAME telepath_observation_reader_test
           COMMAND ${TELEPATH_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/python/observation_reader_test.py)
endif()

add_executable(telepath_observation_test
  cpp/buffer/observation_test.cc
)

target_link_libraries(telepath_observation_test PRIVATE telepath)
target_compile_definitions(telepath_observation_test PRIVATE TELEPATH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(TELEPATH_PYTHON3)
  target_compile_definitions(telepath_observation_test PRIVATE TELEPATH_PYTHON3="${TELEPATH_PYTHON3}")
endif()

add_test(NAME telepath_observation_test COMMAND telepath_observation_test)

add_executable(telepath_observation_stress_test
  cpp/buffer/observation_stress_test.cc
)

target_link_libraries(telepath_observation_stress_test PRIVATE telepath)
target_compile_definitions(telepath_observation_stress_test PRIVATE TELEPATH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(TELEPATH_PYTHON3)
  target_compile_definitions(telepath_observation_stress_test PRIVATE TELEPATH_PYTHON3="${TELEPATH_PYTHON3}")
endif()

add_test(NAME telepath_observation_stress_test COMMAND telepath_observation_stress_test)

add_executable(telepath_benchmark
  cpp/benchmark/benchmark_main.cc
)

target_include_directories(telepath_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/benchmark)
target_link_libraries(telepath_benchmark PRIVATE telepath)

add_dependencies(telepath_benchmark_json_output_test telepath_benchmark)
