#include "scenario_state.h"

#include <sys/wait.h>
#include <toml++/toml.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace kairos::exec {

namespace {

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// Parse "(cum <N> sh" out of a fill line; returns the cumulative shares or -1.
long ParseCumShares(const std::string& line) {
  std::size_t at = line.find("(cum ");
  if (at == std::string::npos) return -1;
  at += 5;
  std::size_t end = at;
  while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
  if (end == at) return -1;
  return std::atol(line.substr(at, end - at).c_str());
}

// Parse the integer percent out of "kairos-exec: progress <pct>%".
int ParseProgressPct(const std::string& line) {
  std::size_t at = line.find("progress ");
  if (at == std::string::npos) return -1;
  at += 9;
  std::size_t end = at;
  while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
  if (end == at) return -1;
  return std::atoi(line.substr(at, end - at).c_str());
}

}  // namespace

const char* StateName(ScenarioState state) {
  switch (state) {
    case ScenarioState::kStopped:
      return "stopped";
    case ScenarioState::kStarting:
      return "starting";
    case ScenarioState::kWaitOpen:
      return "wait-open";
    case ScenarioState::kInWindow:
      return "in-window";
    case ScenarioState::kFillRemainder:
      return "fill-remainder";
    case ScenarioState::kClosedExited:
      return "closed-exited";
    case ScenarioState::kCrashed:
      return "crashed";
    case ScenarioState::kStopping:
      return "stopping";
  }
  return "stopped";
}

ScenarioState ApplyStdoutLine(ScenarioState cur, const std::string& line,
                              ScenarioCounters* counters, long now_ts) {
  if (StartsWith(line, "kairos-exec: fill ")) {
    counters->cum_fills += 1;
    long cum = ParseCumShares(line);
    if (cum >= 0) counters->cum_shares = cum;
    counters->last_fill_ts = now_ts;
    return cur == ScenarioState::kFillRemainder ? cur : ScenarioState::kInWindow;
  }
  if (StartsWith(line, "kairos-exec: progress ")) {
    if (ParseProgressPct(line) >= 100) return ScenarioState::kFillRemainder;
    return cur;
  }
  // The run banner: "kairos-exec: <side> <sym> NT$ <budget>, <policy>, <PAPER|LIVE>".
  if (StartsWith(line, "kairos-exec: ") && line.find(" NT$ ") != std::string::npos &&
      (line.find("PAPER") != std::string::npos || line.find("LIVE") != std::string::npos)) {
    if (cur == ScenarioState::kStarting) return ScenarioState::kWaitOpen;
    return cur;
  }
  return cur;
}

std::string ExtractFailureReason(const std::string& line) {
  constexpr const char* kFatal = "kairos-exec: FATAL ";
  if (StartsWith(line, kFatal)) return line.substr(std::string(kFatal).size());
  if (line.find("order backend connect failed") != std::string::npos)
    return "order backend connect failed";
  if (line.find("order backend disconnected") != std::string::npos)
    return "order backend disconnected";
  if (StartsWith(line, "REFUSING TO TRADE")) return "blacklist gate refused";
  if (StartsWith(line, "scenario invalid")) return "invalid scenario config";
  if (StartsWith(line, "failed to load scenario")) return "failed to load scenario";
  return "";
}

ExitOutcome ClassifyExit(bool requested_stop, int wait_status, bool saw_end_line,
                         const std::string& last_fail_reason) {
  if (requested_stop) return {ScenarioState::kStopped, "stopped by operator"};
  if (WIFSIGNALED(wait_status)) {
    int sig = WTERMSIG(wait_status);
    std::string reason =
        last_fail_reason.empty() ? "killed by signal " + std::to_string(sig) : last_fail_reason;
    return {ScenarioState::kCrashed, reason};
  }
  int code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
  if (code != 0) {
    std::string reason =
        last_fail_reason.empty() ? "exited with code " + std::to_string(code) : last_fail_reason;
    return {ScenarioState::kCrashed, reason};
  }
  // Exit 0 but bailed before the end line with a known fatal line: still a crash.
  if (!saw_end_line && !last_fail_reason.empty()) {
    return {ScenarioState::kCrashed, last_fail_reason};
  }
  return {ScenarioState::kClosedExited,
          saw_end_line ? "window closed / run complete" : "exited cleanly"};
}

std::vector<std::string> BuildTraderArgv(const std::string& bin, const std::string& toml,
                                         ScenarioMode mode) {
  std::vector<std::string> argv{bin, toml};
  switch (mode) {
    case ScenarioMode::kPaper:
      break;
    case ScenarioMode::kLive:
      argv.push_back("--live");
      argv.push_back("--yes");
      break;
    case ScenarioMode::kTest:
      argv.push_back("--ignore-window");
      argv.push_back("--ignore-blacklist");
      break;
  }
  return argv;
}

std::string ResolveTraderBin(const std::string& flag, const char* env,
                             const std::string& scenario_dir) {
  if (!flag.empty()) return flag;
  if (env != nullptr && env[0] != '\0') return env;
  return (std::filesystem::path(scenario_dir) / "build" / "kairos_scenario_trader").string();
}

std::vector<ScenarioInfo> EnumerateScenarios(const std::string& dir) {
  std::vector<ScenarioInfo> out;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) return out;
  for (const auto& entry : it) {
    if (!entry.is_regular_file()) continue;
    std::string name = entry.path().filename().string();
    if (!name.ends_with(".toml") || name.find(".example.") != std::string::npos) continue;
    std::string symbol;
    try {
      auto t = toml::parse_file(entry.path().string());
      symbol = t["scenario"]["symbol"].value<std::string>().value_or("");
    } catch (const toml::parse_error&) {
      continue;  // unparseable toml: skip, never fatal
    }
    if (symbol.empty()) continue;  // config toml (e.g. hub.toml), not launchable
    out.push_back({entry.path().stem().string(), symbol, entry.path().string()});
  }
  std::sort(out.begin(), out.end(),
            [](const ScenarioInfo& a, const ScenarioInfo& b) { return a.path < b.path; });
  return out;
}

}  // namespace kairos::exec
