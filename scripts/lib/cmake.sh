#!/usr/bin/env bash

telepath_root_dir() {
  cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd
}

telepath_build_dir_for_preset() {
  local preset="$1"
  case "${preset}" in
    debug)
      printf '%s/build/debug\n' "${TELEPATH_ROOT_DIR}"
      ;;
    asan)
      printf '%s/build/asan\n' "${TELEPATH_ROOT_DIR}"
      ;;
    lsan)
      printf '%s/build/lsan\n' "${TELEPATH_ROOT_DIR}"
      ;;
    io_uring_debug)
      printf '%s/build/io_uring_debug\n' "${TELEPATH_ROOT_DIR}"
      ;;
    *)
      printf 'unknown preset: %s\n' "${preset}" >&2
      return 1
      ;;
  esac
}

telepath_configure_and_build() {
  local preset="$1"
  local build_dir

  build_dir="$(telepath_build_dir_for_preset "${preset}")"
  mkdir -p "${build_dir}" "${TELEPATH_ROOT_DIR}/build"

  exec 9>"${build_dir}/.telepath.lock"
  flock 9

  (
    cd "${TELEPATH_ROOT_DIR}"
    cmake --preset "${preset}"
    cmake --build --preset "${preset}" -j"$(nproc)"
  )
}

telepath_run_tests() {
  local preset="$1"

  exec 8>"${TELEPATH_ROOT_DIR}/build/.telepath-tests.lock"
  flock 8

  (
    cd "${TELEPATH_ROOT_DIR}"
    ctest --preset "${preset}"
  )
}
