#ifndef TELEPATH_TEST_CPP_IO_IO_TEST_SUPPORT_H_
#define TELEPATH_TEST_CPP_IO_IO_TEST_SUPPORT_H_

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace telepath::io_test_support {

class TestRootGuard {
 public:
  explicit TestRootGuard(std::string name) : root_path_(BuildRootPath(std::move(name))) {
    std::filesystem::remove_all(root_path_);
    std::filesystem::create_directories(root_path_);
  }

  ~TestRootGuard() { std::filesystem::remove_all(root_path_); }

  auto path() const -> const std::filesystem::path & { return root_path_; }

 private:
  static auto BuildRootPath(std::string name) -> std::filesystem::path {
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / (std::move(name) + "_" + std::to_string(unique_suffix));
  }

  std::filesystem::path root_path_;
};

inline bool RequireIoUringSuccess() {
  const char *value = std::getenv("TELEPATH_REQUIRE_IO_URING_SUCCESS");
  return value != nullptr && std::string(value) == "1";
}

}  // namespace telepath::io_test_support

#endif  // TELEPATH_TEST_CPP_IO_IO_TEST_SUPPORT_H_
