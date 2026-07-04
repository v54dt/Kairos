#include "order_journal.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>

namespace kairos::exec {

namespace {
long NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// mkdir -p for a (typically shallow) path.
void MakeDirs(const std::string& dir) {
  std::string acc;
  for (std::size_t i = 0; i < dir.size(); ++i) {
    acc += dir[i];
    if (dir[i] == '/' && acc.size() > 1) ::mkdir(acc.c_str(), 0755);
  }
  ::mkdir(dir.c_str(), 0755);
}
}  // namespace

std::string JournalPath(const std::string& dir, const std::string& name) {
  return dir + "/" + name + ".jsonl";
}

OrderJournal::~OrderJournal() {
  if (f_) std::fclose(f_);
}

bool OrderJournal::Open(const std::string& dir, const std::string& name) {
  MakeDirs(dir);
  f_ = std::fopen(JournalPath(dir, name).c_str(), "ae");  // append, close-on-exec
  return f_ != nullptr;
}

void OrderJournal::Write(const std::string& line) {
  if (!f_) return;
  std::fwrite(line.data(), 1, line.size(), f_);
  std::fflush(f_);
  ::fsync(::fileno(f_));  // a fill must survive the crash that triggers the restart
}

void OrderJournal::LogFill(const std::string& id, long shares, Cents price) {
  Write("{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"fill\",\"id\":\"" + id +
        "\",\"shares\":" + std::to_string(shares) + ",\"price\":" + std::to_string(price) + "}\n");
}

void OrderJournal::LogAck(const std::string& id, bool ok) {
  Write("{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"ack\",\"id\":\"" + id +
        "\",\"ok\":" + (ok ? "1" : "0") + "}\n");
}

void OrderJournal::LogCancel(const std::string& id, bool ok) {
  Write("{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"cancel\",\"id\":\"" + id +
        "\",\"ok\":" + (ok ? "1" : "0") + "}\n");
}

long JournalJsonInt(const std::string& line, const std::string& key, long dflt) {
  std::string needle = "\"" + key + "\":";
  auto p = line.find(needle);
  if (p == std::string::npos) return dflt;
  p += needle.size();
  const char* start = line.c_str() + p;
  char* end = nullptr;
  long v = std::strtol(start, &end, 10);
  return end == start ? dflt : v;
}

std::vector<JournalFill> ReadJournalFills(const std::string& path) {
  std::vector<JournalFill> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("\"type\":\"fill\"") == std::string::npos) continue;
    JournalFill f;
    f.shares = JournalJsonInt(line, "shares", 0);
    f.price = JournalJsonInt(line, "price", 0);
    if (f.shares != 0) out.push_back(f);
  }
  return out;
}

}  // namespace kairos::exec
