#include "predicate_manual.h"

#include <sys/stat.h>

#include <cstdio>
#include <map>
#include <string>
#include <utility>

namespace kairos::exec {

namespace {

// Extract a flat {"k":"v",...} object of string fields. Fail-closed: any
// structural break returns false and the line is skipped rather than crashing.
bool ParseFlatObject(const std::string& line, std::map<std::string, std::string>* out) {
  std::size_t i = 0;
  auto skip_ws = [&] {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
  };
  auto read_string = [&](std::string* s) {
    if (i >= line.size() || line[i] != '"') return false;
    ++i;
    while (i < line.size()) {
      char c = line[i++];
      if (c == '"') return true;
      if (c == '\\') {
        if (i >= line.size()) return false;
        char e = line[i++];
        switch (e) {
          case 'n':
            *s += '\n';
            break;
          case 't':
            *s += '\t';
            break;
          case 'r':
            *s += '\r';
            break;
          default:
            *s += e;
        }
        continue;
      }
      *s += c;
    }
    return false;
  };
  skip_ws();
  if (i >= line.size() || line[i] != '{') return false;
  ++i;
  skip_ws();
  if (i < line.size() && line[i] == '}') return true;
  while (true) {
    skip_ws();
    std::string key;
    if (!read_string(&key)) return false;
    skip_ws();
    if (i >= line.size() || line[i] != ':') return false;
    ++i;
    skip_ws();
    std::string val;
    if (!read_string(&val)) return false;
    (*out)[key] = val;
    skip_ws();
    if (i >= line.size()) return false;
    if (line[i] == ',') {
      ++i;
      continue;
    }
    if (line[i] == '}') return true;
    return false;
  }
}

}  // namespace

ManualPredicate::ManualPredicate(std::string name, std::vector<std::string> symbols,
                                 std::string spool_path)
    : Predicate(std::move(name), std::move(symbols)), spool_path_(std::move(spool_path)) {}

std::vector<PredicateHit> ManualPredicate::Poll(std::int64_t /*ts_us*/) {
  std::vector<PredicateHit> hits;
  struct stat st;
  if (::stat(spool_path_.c_str(), &st) != 0) return hits;
  auto size = static_cast<std::uint64_t>(st.st_size);
  if (size < offset_) offset_ = 0;  // file was truncated/replaced
  if (size == offset_) return hits;

  std::FILE* f = std::fopen(spool_path_.c_str(), "rb");
  if (f == nullptr) return hits;
  if (std::fseek(f, static_cast<long>(offset_), SEEK_SET) != 0) {
    std::fclose(f);
    return hits;
  }

  std::string line;
  int c;
  while ((c = std::fgetc(f)) != EOF) {
    ++offset_;
    if (c == '\n') {
      std::map<std::string, std::string> obj;
      if (ParseFlatObject(line, &obj)) {
        auto sig = obj.find("signal");
        auto sym = obj.find("symbol");
        auto act = obj.find("action");
        if (sig != obj.end() && sym != obj.end() && act != obj.end() && sig->second == name_ &&
            WatchesSymbol(sym->second)) {
          SignalAction action;
          if (ParseSignalAction(act->second, &action)) {
            PredicateHit hit;
            hit.symbol = sym->second;
            hit.fire.action = action;
            hits.push_back(std::move(hit));
          }
        }
      }
      line.clear();
      continue;
    }
    line += static_cast<char>(c);
  }
  // A trailing partial line (no newline yet) stays unconsumed: rewind past it so
  // the next poll re-reads it once the appender finishes the line.
  offset_ -= line.size();
  std::fclose(f);
  return hits;
}

}  // namespace kairos::exec
