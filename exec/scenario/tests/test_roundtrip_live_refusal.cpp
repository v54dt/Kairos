// LIVE + [roundtrip] must refuse at startup (fail-closed until journal-based restart
// recovery lands). Spawns the real kairos_scenario_trader binary with a roundtrip
// scenario in --live mode and asserts a non-zero exit plus a refusal naming roundtrip.
// Self-skips (exit 0) if the trader binary is not next to this test binary.

#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string SelfDir() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  return std::filesystem::path(buf).parent_path().string();
}

const char* kScenario = R"([scenario]
name = "rt-live-refusal"
symbol = "2330"
market = "TSE"
board = "OddLot"
side = "Buy"
budget_twd = 100000
pacing = "twap"

[roundtrip]
enabled = true
signal = "vwap_reversion"
stop_loss_pct = 1.0
max_hold_min = 30
enter_window_min = 10
arm_start = "09:00"
arm_end = "12:00"
)";

}  // namespace

int main() {
  std::string trader = SelfDir() + "/kairos_scenario_trader";
  if (!std::filesystem::exists(trader)) {
    std::printf("test_roundtrip_live_refusal: SKIP (no trader binary)\n");
    return 0;
  }

  std::filesystem::path toml =
      std::filesystem::temp_directory_path() / "kairos_rt_live_refusal.toml";
  {
    std::ofstream out(toml);
    out << kScenario;
  }

  // --ignore-blacklist --yes clears the fail-closed blacklist gate (no CSV present)
  // so the run reaches the roundtrip live check; stdin is closed so a stray prompt
  // cannot hang. The refusal fires before any order path.
  std::string cmd =
      "'" + trader + "' '" + toml.string() + "' --live --ignore-blacklist --yes </dev/null 2>&1";
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) {
    std::printf("test_roundtrip_live_refusal: FAILED popen\n");
    return 1;
  }
  std::string output;
  std::array<char, 512> chunk{};
  while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), pipe)) output += chunk.data();
  int status = ::pclose(pipe);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  std::filesystem::remove(toml);

  bool refused = code != 0 && output.find("roundtrip") != std::string::npos;
  if (refused) {
    std::printf("test_roundtrip_live_refusal: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_live_refusal: FAILED (code=%d, output=%s)\n", code, output.c_str());
  return 1;
}
