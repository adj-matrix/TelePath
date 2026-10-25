#!/usr/bin/env bash

TELEPATH_DEFAULT_CXX="${TELEPATH_DEFAULT_CXX:-g++-14}"
TELEPATH_DEFAULT_CC="${TELEPATH_DEFAULT_CC:-gcc-14}"
TELEPATH_CLANG_CXX="${TELEPATH_CLANG_CXX:-clang++-19}"
TELEPATH_CLANG_CC="${TELEPATH_CLANG_CC:-clang-19}"

telepath_root_dir() {
  cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd
}

telepath_build_dir_for_preset() {
  local preset="$1"
  case "${preset}" in
    debug|asan|lsan|tsan|release|io_uring_debug|io_uring_release)
      printf '%s/build/%s\n' "${TELEPATH_ROOT_DIR}" "${preset}"
      ;;
    *)
      printf 'unknown preset: %s\n' "${preset}" >&2
      return 1
      ;;
  esac
}

telepath_select_compiler() {
  local preset="$1"
  if [[ -n "${CXX:-}" ]]; then
    return
  fi
  if [[ "${preset}" == "tsan" ]]; then
    export CXX="${TELEPATH_CLANG_CXX}" CC="${TELEPATH_CLANG_CC}"
    return
  fi
  export CXX="${TELEPATH_DEFAULT_CXX}" CC="${TELEPATH_DEFAULT_CC}"
}

telepath_reset_stale_cache() {
  local build_dir="$1"
  local cache="${build_dir}/CMakeCache.txt"
  [[ -f "${cache}" ]] || return 0

  local cached requested
  cached="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "${cache}")"
  requested="$(command -v "${CXX}" || true)"
  [[ -n "${cached}" && -n "${requested}" ]] || return 0
  if [[ "$(readlink -f "${cached}")" != "$(readlink -f "${requested}")" ]]; then
    echo "Compiler changed (${cached} -> ${requested}); reconfiguring ${build_dir}" >&2
    rm -rf "${cache}" "${build_dir}/CMakeFiles"
  fi
}

telepath_configure_and_build() {
  local preset="$1"
  local build_dir

  build_dir="$(telepath_build_dir_for_preset "${preset}")"
  mkdir -p "${build_dir}" "${TELEPATH_ROOT_DIR}/build"

  exec 9>"${build_dir}/.telepath.lock"
  flock 9

  telepath_select_compiler "${preset}"
  telepath_reset_stale_cache "${build_dir}"

  (
    cd "${TELEPATH_ROOT_DIR}"
    cmake --preset "${preset}"
    cmake --build --preset "${preset}" -j"$(nproc)"
  )

  if [[ "${preset}" == "debug" ]]; then
    ln -sfn "${build_dir}/compile_commands.json" "${TELEPATH_ROOT_DIR}/build/compile_commands.json"
  fi
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
