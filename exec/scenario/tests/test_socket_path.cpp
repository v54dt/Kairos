#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "socket_path.h"
#include "test_check.h"

// Cross-language golden: every row of schema/testdata/runtime_paths.txt must
// resolve identically through ResolveSock here, core resolve() (Rust), and the
// three tui resolvers. See the fixture header for the column contract.

namespace {

using kairos::exec::ResolveSock;

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

}  // namespace

int main() {
  std::ifstream in(KAIROS_RUNTIME_PATHS_PATH);
  CHECK(in.good());
  std::string line;
  int rows = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f = Split(line, '|');
    CHECK_EQ(static_cast<int>(f.size()), 5);
    if (f.size() != 5) continue;
    const std::string& env = f[0];
    const std::string& xdg = f[1];
    const std::string& run_user = f[2];
    const std::string& base = f[3];
    const std::string& expected = f[4];

    const char* env_p = (env == "UNSET") ? nullptr : (env == "EMPTY" ? "" : env.c_str());
    const char* xdg_p = (xdg == "UNSET") ? nullptr : (xdg == "EMPTY" ? "" : xdg.c_str());
    std::string ru = (run_user == "yes") ? "/run/user/1000" : "";
    std::string want = (expected == "FATAL") ? "" : expected;

    std::string got = ResolveSock(env_p, xdg_p, ru, base.c_str());
    if (got != want) {
      std::printf("FAIL  row=%s  got=%s  want=%s\n", line.c_str(), got.c_str(), want.c_str());
      return 1;
    }
    ++rows;
  }
  CHECK(rows >= 40);
  if (g_failures != 0) std::printf("%d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
