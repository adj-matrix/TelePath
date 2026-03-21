cmake_minimum_required(VERSION 3.16)

project(
    TelePath
    VERSION 0.1.0
    DESCRIPTION "Observable buffer pool SDK for modern storage"
    LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(TELEPATH_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(TELEPATH_ENABLE_LSAN "Enable LeakSanitizer" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(TELEPATH_SANITIZER_FLAGS)
if(TELEPATH_ENABLE_ASAN)
  list(APPEND TELEPATH_SANITIZER_FLAGS -fsanitize=address)
endif()
if(TELEPATH_ENABLE_LSAN)
  list(APPEND TELEPATH_SANITIZER_FLAGS -fsanitize=leak)
endif()

if(TELEPATH_SANITIZER_FLAGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(${TELEPATH_SANITIZER_FLAGS} -fno-omit-frame-pointer)
  add_link_options(${TELEPATH_SANITIZER_FLAGS} -fno-omit-frame-pointer)
endif()

enable_testing()

add_subdirectory(src/cpp)
add_subdirectory(test)
