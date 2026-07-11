#ifndef KAIROS_EXEC_JSON_UTIL_H_
#define KAIROS_EXEC_JSON_UTIL_H_

#include <string>

namespace kairos::exec {

// Full JSON string escaping: every character that would break a JSON string
// literal, including the C0 control chars as \u00xx (untrusted broker/exit text
// may carry newlines/tabs/control bytes). UTF-8 bytes >= 0x20 pass through so
// Chinese survives. Byte-identical to order_journal.cpp AppendJsonEscaped.
inline std::string JsonEscape(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
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
        if (u < 0x20) {
          out += "\\u00";
          out += kHex[u >> 4];
          out += kHex[u & 0xF];
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
