// LIVE + [roundtrip] gating around restart recovery. Supersedes the PR4 blanket
// refusal: live now REQUIRES a writable rt journal. Spawns the real trader binary
// with a roundtrip scenario in --live mode and asserts two cases:
//   (A) no writable journal (KAIROS_JOURNAL_DIR points into a read-only path) =>
//       refuse at startup with kNoJournalExit and a message naming the journal;
//   (B) a writable journal dir => the gate passes and the runner starts (it then
//       blocks on the absent signal/quote sockets, so a timeout kills it: a
//       124 exit means it ran past the gate and never emitted the refusal).
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

struct RunResult {
  int code;
  std::string output;
};

// Runs the trader with KAIROS_JOURNAL_DIR=<journal_dir> under `timeout secs`, stdin
// closed. Returns the shell exit code (124 when the timeout killed a live process).
RunResult Run(const std::string& trader, const std::string& toml, const std::string& journal_dir,
              int secs) {
  std::string cmd = "KAIROS_JOURNAL_DIR='" + journal_dir + "' timeout " + std::to_string(secs) +
                    " '" + trader + "' '" + toml +
                    "' --live --ignore-blacklist --yes </dev/null 2>&1";
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) return {-1, ""};
  std::string output;
  std::array<char, 512> chunk{};
  while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), pipe)) output += chunk.data();
  int status = ::pclose(pipe);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

}  // namespace

int main() {
  std::string trader = SelfDir() + "/kairos_scenario_trader";
  if (!std::filesystem::exists(trader)) {
    std::printf("test_roundtrip_live_refusal: SKIP (no trader binary)\n");
    return 0;
  }

  const std::string tag = std::to_string(::getpid());
  std::filesystem::path toml =
      std::filesystem::temp_directory_path() / ("kairos_rt_live_" + tag + ".toml");
  {
    std::ofstream out(toml);
    out << kScenario;
  }

  int rc = 0;

  // (A) No writable journal: a read-only /proc path defeats ResolveJournalDir's
  // $HOME fallback, so the append probe fails and the run must refuse (kNoJournalExit
  // == 2) before any order path.
  {
    RunResult r = Run(trader, toml.string(), "/proc/kairos-rt-" + tag, 5);
    bool refused = r.code == 2 && r.output.find("roundtrip") != std::string::npos &&
                   r.output.find("journal") != std::string::npos;
    if (!refused) {
      std::printf("test_roundtrip_live_refusal: FAIL A (code=%d, output=%s)\n", r.code,
                  r.output.c_str());
      rc = 1;
    }
  }

  // (B) Writable journal dir: the gate passes and the runner starts, then blocks on
  // the missing sockets until the 2s timeout (exit 124). It must NOT have refused.
  {
    std::filesystem::path jdir = std::filesystem::temp_directory_path() / ("kairos-rt-jdir-" + tag);
    std::filesystem::remove_all(jdir);
    std::filesystem::create_directories(jdir);
    RunResult r = Run(trader, toml.string(), jdir.string(), 2);
    bool started = r.code == 124 && r.output.find("FATAL live [roundtrip]") == std::string::npos;
    if (!started) {
      std::printf("test_roundtrip_live_refusal: FAIL B (code=%d, output=%s)\n", r.code,
                  r.output.c_str());
      rc = 1;
    }
    std::filesystem::remove_all(jdir);
  }

  std::filesystem::remove(toml);
  if (rc == 0) std::printf("test_roundtrip_live_refusal: OK\n");
  return rc;
}
