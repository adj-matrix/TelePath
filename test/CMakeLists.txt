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
