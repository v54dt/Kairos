#include "scenario_ctl_proto.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace kairos::exec {

namespace {

// Escape the few characters that would break a JSON string literal.
std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// Read one JSON string starting at s[*i] == '"'; append its unescaped content to
// *out and advance *i past the closing quote. Returns false on an unterminated
// or malformed string.
bool ParseJsonString(const std::string& s, std::size_t* i, std::string* out) {
  if (*i >= s.size() || s[*i] != '"') return false;
  ++*i;
  while (*i < s.size()) {
    char c = s[*i];
    if (c == '"') {
      ++*i;
      return true;
    }
    if (c == '\\') {
      if (*i + 1 >= s.size()) return false;
      char e = s[*i + 1];
      switch (e) {
        case '"':
          *out += '"';
          break;
        case '\\':
          *out += '\\';
          break;
        case '/':
          *out += '/';
          break;
        case 'n':
          *out += '\n';
          break;
        case 't':
          *out += '\t';
          break;
        default:
          return false;
      }
      *i += 2;
      continue;
    }
    *out += c;
    ++*i;
  }
  return false;  // unterminated
}

void SkipWs(const std::string& s, std::size_t* i) {
  while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r' || s[*i] == '\n')) ++*i;
}

// Decode a flat JSON object of string->string pairs. Rejects nested objects,
// arrays, and non-string values -- the control shape is flat by design.
bool ParseFlatObject(const std::string& line, std::unordered_map<std::string, std::string>* out) {
  std::size_t i = 0;
  SkipWs(line, &i);
  if (i >= line.size() || line[i] != '{') return false;
  ++i;
  SkipWs(line, &i);
  if (i < line.size() && line[i] == '}') {
    ++i;
    SkipWs(line, &i);
    return i == line.size();
  }
  while (true) {
    SkipWs(line, &i);
    std::string key;
    if (!ParseJsonString(line, &i, &key)) return false;
    SkipWs(line, &i);
    if (i >= line.size() || line[i] != ':') return false;
    ++i;
    SkipWs(line, &i);
    std::string val;
    if (!ParseJsonString(line, &i, &val)) return false;
    (*out)[key] = val;
    SkipWs(line, &i);
    if (i >= line.size()) return false;
    if (line[i] == ',') {
      ++i;
      continue;
    }
    if (line[i] == '}') {
      ++i;
      break;
    }
    return false;
  }
  SkipWs(line, &i);
  return i == line.size();
}

}  // namespace

bool ParseScenarioMode(const std::string& token, ScenarioMode* out) {
  if (token == "paper") {
    *out = ScenarioMode::kPaper;
    return true;
  }
  if (token == "live") {
    *out = ScenarioMode::kLive;
    return true;
  }
  if (token == "test") {
    *out = ScenarioMode::kTest;
    return true;
  }
  return false;
}

const char* ScenarioModeName(ScenarioMode mode) {
  switch (mode) {
    case ScenarioMode::kPaper:
      return "paper";
    case ScenarioMode::kLive:
      return "live";
    case ScenarioMode::kTest:
      return "test";
  }
  return "paper";
}

bool ParseScenarioRequest(const std::string& line, ScenarioRequest* out, std::string* err) {
  if (line.size() > kMaxCtlLineLen) {
    *err = "line too long";
    return false;
  }
  std::unordered_map<std::string, std::string> obj;
  if (!ParseFlatObject(line, &obj)) {
    *err = "malformed request";
    return false;
  }
  auto cmd = obj.find("cmd");
  if (cmd == obj.end()) {
    *err = "missing cmd";
    return false;
  }
  if (cmd->second == "list") {
    out->cmd = ScenarioCmd::kList;
    out->name.clear();
    return true;
  }
  if (cmd->second == "start" || cmd->second == "stop") {
    auto name = obj.find("name");
    if (name == obj.end() || name->second.empty()) {
      *err = "missing name";
      return false;
    }
    out->name = name->second;
    if (cmd->second == "stop") {
      out->cmd = ScenarioCmd::kStop;
      return true;
    }
    out->cmd = ScenarioCmd::kStart;
    auto mode = obj.find("mode");
    if (mode == obj.end()) {
      *err = "missing mode";
      return false;
    }
    if (!ParseScenarioMode(mode->second, &out->mode)) {
      *err = "unknown mode";
      return false;
    }
    return true;
  }
  *err = "unknown cmd";
  return false;
}

std::string SerializeScenarioSnapshot(bool ok, const std::string& err,
                                      const std::vector<ScenarioSnapshotRow>& rows) {
  std::string out = "{\"ok\":";
  out += ok ? "true" : "false";
  out += ",\"err\":\"" + JsonEscape(err) + "\",\"scenarios\":[";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const ScenarioSnapshotRow& r = rows[i];
    if (i != 0) out += ',';
    out += "{\"name\":\"" + JsonEscape(r.name) + "\",\"state\":\"" + JsonEscape(r.state) +
           "\",\"pid\":" + std::to_string(r.pid) + ",\"cum_fills\":" + std::to_string(r.cum_fills) +
           ",\"cum_shares\":" + std::to_string(r.cum_shares) +
           ",\"last_fill_ts\":" + std::to_string(r.last_fill_ts) + ",\"last_exit_reason\":\"" +
           JsonEscape(r.last_exit_reason) + "\",\"live\":" + (r.live ? "true" : "false") +
           ",\"restart_count\":" + std::to_string(r.restart_count) +
           ",\"gave_up\":" + (r.gave_up ? "true" : "false") + "}";
  }
  out += "]}\n";
  return out;
}

}  // namespace kairos::exec
