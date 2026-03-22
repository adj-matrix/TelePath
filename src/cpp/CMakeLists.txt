set(TELEPATH_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(telepath
  lib/buffer/buffer_manager.cc
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

target_compile_features(telepath PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(telepath PRIVATE -Werror)
endif()
