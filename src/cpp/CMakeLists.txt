set(TELEPATH_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(telepath
  lib/buffer/buffer_manager.cc
  lib/io/disk_backend_factory.cc
  lib/io/io_uring_disk_backend.cc
  lib/io/posix_disk_backend.cc
  lib/replacer/clock_replacer.cc
  lib/replacer/lru_k_replacer.cc
  lib/replacer/lru_replacer.cc
  lib/telemetry/counter_telemetry_sink.cc
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

target_compile_features(telepath PUBLIC cxx_std_17)
target_compile_definitions(telepath
  PUBLIC
    TELEPATH_HAS_LIBURING=$<BOOL:${TELEPATH_HAS_LIBURING}>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(telepath PRIVATE -Werror)
endif()
