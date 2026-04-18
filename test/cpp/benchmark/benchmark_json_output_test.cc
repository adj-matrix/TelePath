#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <string>

namespace {

auto BuildBenchmarkCommand(const std::filesystem::path &benchmark_bin) -> std::string {
  return benchmark_bin.string() +
         " --output-format json --workload hotspot --pool-size 16"
         " --block-count 32 --threads 2 --ops-per-thread 12"
         " --hotset-size 8 --hot-access-percent 80";
}

auto ReadProcessOutput(const std::string &command, int *exit_code) -> std::string {
  FILE *pipe = popen(command.c_str(), "r");
  assert(pipe != nullptr);

  std::string output;
  std::array<char, 512> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  *exit_code = pclose(pipe);
  return output;
}

auto ExtractTotalOps(const std::string &output) -> uint64_t {
  const std::regex total_ops_pattern(R"("total_ops":\s*([0-9]+))");
  std::smatch total_ops_match;
  assert(std::regex_search(output, total_ops_match, total_ops_pattern));
  return std::stoull(total_ops_match[1].str());
}

auto SumObservedAccesses(const std::string &output) -> uint64_t {
  const std::regex access_pattern(R"("accesses":\s*([0-9]+))");
  uint64_t access_sum = 0;
  for (std::sregex_iterator iter(output.begin(), output.end(), access_pattern); iter != std::sregex_iterator(); ++iter) {
    access_sum += std::stoull((*iter)[1].str());
  }
  return access_sum;
}

}  // namespace

int main(int /*argc*/, char **argv) {
  namespace fs = std::filesystem;

  const fs::path executable_dir = fs::path(argv[0]).parent_path();
  const fs::path benchmark_bin = executable_dir / "telepath_benchmark";
  const std::string command = BuildBenchmarkCommand(benchmark_bin);

  int exit_code = 0;
  const std::string output = ReadProcessOutput(command, &exit_code);
  assert(exit_code == 0);

  assert(output.find("\"access_profile\"") != std::string::npos);
  assert(output.find("\"mode\": \"observed\"") != std::string::npos);

  const uint64_t total_ops = ExtractTotalOps(output);
  const uint64_t access_sum = SumObservedAccesses(output);
  assert(access_sum == total_ops);
  return 0;
}
