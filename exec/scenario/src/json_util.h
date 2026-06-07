#ifndef KAIROS_EXEC_JSON_UTIL_H_
#define KAIROS_EXEC_JSON_UTIL_H_

#include <cstdio>
#include <string>

namespace kairos::exec {

// Minimal JSON string escaping (UTF-8 bytes pass through so Chinese survives).
inline std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

inline std::string JsonString(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_JSON_UTIL_H_
