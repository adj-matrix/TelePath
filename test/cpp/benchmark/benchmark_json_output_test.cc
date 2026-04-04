#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <string>

int main(int /*argc*/, char **argv) {
  namespace fs = std::filesystem;

  const fs::path executable_dir = fs::path(argv[0]).parent_path();
  const fs::path benchmark_bin = executable_dir / "telepath_benchmark";
  const std::string command =
      benchmark_bin.string() +
      " --output-format json --workload hotspot --pool-size 16 --block-count 32"
      " --threads 2 --ops-per-thread 12 --hotset-size 8 --hot-access-percent 80";

  FILE *pipe = popen(command.c_str(), "r");
  assert(pipe != nullptr);

  std::string output;
  std::array<char, 512> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  const int exit_code = pclose(pipe);
  assert(exit_code == 0);

  assert(output.find("\"access_profile\"") != std::string::npos);
  assert(output.find("\"mode\": \"observed\"") != std::string::npos);

  const std::regex total_ops_pattern(R"("total_ops":\s*([0-9]+))");
  std::smatch total_ops_match;
  assert(std::regex_search(output, total_ops_match, total_ops_pattern));
  const uint64_t total_ops = std::stoull(total_ops_match[1].str());

  const std::regex access_pattern(R"("accesses":\s*([0-9]+))");
  uint64_t access_sum = 0;
  for (std::sregex_iterator iter(output.begin(), output.end(), access_pattern);
       iter != std::sregex_iterator(); ++iter) {
    access_sum += std::stoull((*iter)[1].str());
  }

  assert(access_sum == total_ops);
  return 0;
}
