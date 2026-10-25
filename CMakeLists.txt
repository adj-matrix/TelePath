cmake_minimum_required(VERSION 3.16)

project(
    TelePath
    VERSION 0.1.0
    DESCRIPTION "Observable buffer pool SDK for modern storage"
    LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "TelePath requires GCC 14 or newer (found ${CMAKE_CXX_COMPILER_VERSION}); run support/install_build_deps.sh")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
  message(FATAL_ERROR "TelePath requires Clang 19 or newer (found ${CMAKE_CXX_COMPILER_VERSION}); run support/install_build_deps.sh")
endif()

option(TELEPATH_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(TELEPATH_ENABLE_LSAN "Enable LeakSanitizer" OFF)
option(TELEPATH_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(TELEPATH_ENABLE_IO_URING "Enable liburing-backed build integration" OFF)

set(TELEPATH_HAS_LIBURING OFF)
set(TELEPATH_LIBURING_INCLUDE_DIR "")
set(TELEPATH_LIBURING_LIBRARY "")

if(TELEPATH_ENABLE_IO_URING)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PC_LIBURING QUIET liburing)
  if(PC_LIBURING_FOUND)
    set(TELEPATH_LIBURING_INCLUDE_DIR ${PC_LIBURING_INCLUDE_DIRS})
    set(TELEPATH_LIBURING_LIBRARY ${PC_LIBURING_LINK_LIBRARIES})
  endif()
  find_path(TELEPATH_LIBURING_INCLUDE_DIR liburing.h)
  find_library(TELEPATH_LIBURING_LIBRARY uring)
  if(TELEPATH_LIBURING_INCLUDE_DIR AND TELEPATH_LIBURING_LIBRARY)
    set(TELEPATH_HAS_LIBURING ON)
  else()
    message(FATAL_ERROR
      "TELEPATH_ENABLE_IO_URING=ON requires liburing headers and library")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
  add_compile_options($<$<CONFIG:Debug>:-D_GLIBCXX_ASSERTIONS>)
endif()

set(TELEPATH_SANITIZER_FLAGS)
if(TELEPATH_ENABLE_ASAN)
  list(APPEND TELEPATH_SANITIZER_FLAGS -fsanitize=address)
endif()
if(TELEPATH_ENABLE_LSAN)
  list(APPEND TELEPATH_SANITIZER_FLAGS -fsanitize=leak)
endif()
if(TELEPATH_ENABLE_TSAN)
  if(TELEPATH_ENABLE_ASAN OR TELEPATH_ENABLE_LSAN)
    message(FATAL_ERROR "TELEPATH_ENABLE_TSAN cannot be combined with ASAN or LSAN")
  endif()
  list(APPEND TELEPATH_SANITIZER_FLAGS -fsanitize=thread)
endif()

if(TELEPATH_SANITIZER_FLAGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(${TELEPATH_SANITIZER_FLAGS} -fno-omit-frame-pointer)
  add_link_options(${TELEPATH_SANITIZER_FLAGS} -fno-omit-frame-pointer)
endif()

enable_testing()

add_subdirectory(src/cpp)
add_subdirectory(test)
