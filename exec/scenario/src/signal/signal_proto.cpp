#include "signal_proto.h"

#include <cstdint>
#include <map>
#include <string>

#include "json_util.h"

namespace kairos::exec {

namespace {

// Hex digit value, or -1 for a non-hex char.
int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Append the BMP code point `cp` to *out as UTF-8 (1-3 bytes).
void AppendUtf8(unsigned cp, std::string* out) {
  if (cp < 0x80) {
    *out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    *out += static_cast<char>(0xC0 | (cp >> 6));
    *out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    *out += static_cast<char>(0xE0 | (cp >> 12));
    *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Read one JSON string starting at s[*i] == '"'; append its unescaped content to
// *out and advance *i past the closing quote. Accepts every escape JsonEscape can
// produce: \" \\ \/ \b \f \n \r \t and \uXXXX (BMP, surrogates rejected). Returns
// false on an unterminated or malformed string.
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
        case 'b':
          *out += '\b';
          break;
        case 'f':
          *out += '\f';
          break;
        case 'n':
          *out += '\n';
          break;
        case 'r':
          *out += '\r';
          break;
        case 't':
          *out += '\t';
          break;
        case 'u': {
          if (*i + 5 >= s.size()) return false;
          int h3 = HexVal(s[*i + 2]), h2 = HexVal(s[*i + 3]), h1 = HexVal(s[*i + 4]),
              h0 = HexVal(s[*i + 5]);
          if (h3 < 0 || h2 < 0 || h1 < 0 || h0 < 0) return false;
          unsigned cp = (h3 << 12) | (h2 << 8) | (h1 << 4) | h0;
          if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // lone surrogate
          AppendUtf8(cp, out);
          *i += 6;
          continue;
        }
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

// Strict unsigned integer from a validated token: digits only, no leading zero
// (except "0"), overflow-checked.
bool ParseU64(const std::string& tok, std::uint64_t* out) {
  if (tok.empty() || tok[0] == '-') return false;
  if (tok.size() > 1 && tok[0] == '0') return false;
  std::uint64_t v = 0;
  for (char c : tok) {
    if (c < '0' || c > '9') return false;
    std::uint64_t d = static_cast<std::uint64_t>(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  *out = v;
  return true;
}

// Strict signed integer from a validated token: optional '-', digits only, no
// leading zero, overflow-checked against the int64 range.
bool ParseI64(const std::string& tok, std::int64_t* out) {
  bool neg = !tok.empty() && tok[0] == '-';
  std::string digits = neg ? tok.substr(1) : tok;
  if (digits.empty()) return false;
  if (digits.size() > 1 && digits[0] == '0') return false;
  std::uint64_t v = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') return false;
    std::uint64_t d = static_cast<std::uint64_t>(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  std::uint64_t limit =
      neg ? static_cast<std::uint64_t>(INT64_MAX) + 1 : static_cast<std::uint64_t>(INT64_MAX);
  if (v > limit) return false;
  if (neg) {
    *out = (v == static_cast<std::uint64_t>(INT64_MAX) + 1) ? INT64_MIN
                                                            : -static_cast<std::int64_t>(v);
  } else {
    *out = static_cast<std::int64_t>(v);
  }
  return true;
}

// Read one integer token (optional '-' then digits) into *tok and advance *i.
// Rejects floats/exponents and multi-digit leading zeros.
bool ParseNumberToken(const std::string& s, std::size_t* i, std::string* tok) {
  std::size_t start = *i;
  if (*i < s.size() && s[*i] == '-') ++*i;
  std::size_t digits_start = *i;
  while (*i < s.size() && s[*i] >= '0' && s[*i] <= '9') ++*i;
  if (*i == digits_start) return false;
  if (*i < s.size() && (s[*i] == '.' || s[*i] == 'e' || s[*i] == 'E')) return false;
  std::string t = s.substr(start, *i - start);
  const std::string& d = s[start] == '-' ? t.substr(1) : t;
  if (d.size() > 1 && d[0] == '0') return false;
  *tok = std::move(t);
  return true;
}

enum class JsonType { kString, kInt, kBool, kObject };

struct JsonValue {
  JsonType type = JsonType::kString;
  std::string str;                         // kString
  std::string num;                         // kInt (raw validated token)
  bool boolean = false;                    // kBool
  std::map<std::string, std::string> obj;  // kObject (flat string map)
};

// Read a flat string->string object starting at s[*i] == '{'; advance *i past the
// closing brace. Non-string values, nested objects, and arrays are rejected.
bool ParseFlatStringObject(const std::string& s, std::size_t* i,
                           std::map<std::string, std::string>* out) {
  ++*i;  // consume '{'
  SkipWs(s, i);
  if (*i < s.size() && s[*i] == '}') {
    ++*i;
    return true;
  }
  while (true) {
    SkipWs(s, i);
    std::string key;
    if (!ParseJsonString(s, i, &key)) return false;
    SkipWs(s, i);
    if (*i >= s.size() || s[*i] != ':') return false;
    ++*i;
    SkipWs(s, i);
    std::string val;
    if (!ParseJsonString(s, i, &val)) return false;
    (*out)[key] = val;
    SkipWs(s, i);
    if (*i >= s.size()) return false;
    if (s[*i] == ',') {
      ++*i;
      continue;
    }
    if (s[*i] == '}') {
      ++*i;
      break;
    }
    return false;
  }
  return true;
}

// Read one JSON value: string, integer, true/false, or a one-level flat object.
// Arrays, null, floats, and deeper nesting are rejected.
bool ParseValue(const std::string& s, std::size_t* i, JsonValue* v) {
  SkipWs(s, i);
  if (*i >= s.size()) return false;
  char c = s[*i];
  if (c == '"') {
    v->type = JsonType::kString;
    return ParseJsonString(s, i, &v->str);
  }
  if (c == '{') {
    v->type = JsonType::kObject;
    return ParseFlatStringObject(s, i, &v->obj);
  }
  if (c == 't') {
    if (s.compare(*i, 4, "true") == 0) {
      v->type = JsonType::kBool;
      v->boolean = true;
      *i += 4;
      return true;
    }
    return false;
  }
  if (c == 'f') {
    if (s.compare(*i, 5, "false") == 0) {
      v->type = JsonType::kBool;
      v->boolean = false;
      *i += 5;
      return true;
    }
    return false;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    v->type = JsonType::kInt;
    return ParseNumberToken(s, i, &v->num);
  }
  return false;
}

// Decode a top-level JSON object into a key -> typed value map. Unknown keys are
// preserved for the field extractors to ignore; the whole line must be consumed.
bool ParseObject(const std::string& line, std::map<std::string, JsonValue>* out) {
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
    JsonValue v;
    if (!ParseValue(line, &i, &v)) return false;
    (*out)[key] = std::move(v);
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

const JsonValue* Get(const std::map<std::string, JsonValue>& o, const std::string& k) {
  auto it = o.find(k);
  return it == o.end() ? nullptr : &it->second;
}

}  // namespace

bool ParseSignalAction(const std::string& token, SignalAction* out) {
  if (token == "enter") {
    *out = SignalAction::kEnter;
    return true;
  }
  if (token == "exit") {
    *out = SignalAction::kExit;
    return true;
  }
  return false;
}

const char* SignalActionName(SignalAction action) {
  switch (action) {
    case SignalAction::kEnter:
      return "enter";
    case SignalAction::kExit:
      return "exit";
  }
  return "enter";
}

std::string SerializeSubscribe(const SignalSubscribe& sub) {
  return "{\"cmd\":\"sub\",\"signal\":\"" + JsonEscape(sub.signal) + "\",\"symbol\":\"" +
         JsonEscape(sub.symbol) + "\"}\n";
}

std::string SerializeAck(const SignalAck& ack) {
  std::string out = "{\"type\":\"sub_ack\",\"ok\":";
  out += ack.ok ? "true" : "false";
  out += ",\"err\":\"" + JsonEscape(ack.err) + "\"}\n";
  return out;
}

std::string SerializeSignal(const SignalPush& push) {
  std::string out = "{\"type\":\"signal\",\"signal\":\"" + JsonEscape(push.signal) +
                    "\",\"symbol\":\"" + JsonEscape(push.symbol) + "\",\"action\":\"" +
                    SignalActionName(push.action) + "\",\"seq\":" + std::to_string(push.seq) +
                    ",\"ts_us\":" + std::to_string(push.ts_us);
  if (!push.fields.empty()) {
    out += ",\"fields\":{";
    bool first = true;
    for (const auto& [k, v] : push.fields) {
      if (!first) out += ',';
      out += "\"" + JsonEscape(k) + "\":\"" + JsonEscape(v) + "\"";
      first = false;
    }
    out += "}";
  }
  out += "}\n";
  return out;
}

std::string SerializeHeartbeat(const SignalHeartbeat& hb) {
  return "{\"type\":\"hb\",\"seq\":" + std::to_string(hb.seq) +
         ",\"ts_us\":" + std::to_string(hb.ts_us) + "}\n";
}

bool ParseSubscribe(const std::string& line, SignalSubscribe* out, std::string* err) {
  if (line.size() > kMaxSignalLineLen) {
    *err = "line too long";
    return false;
  }
  std::map<std::string, JsonValue> o;
  if (!ParseObject(line, &o)) {
    *err = "malformed";
    return false;
  }
  const JsonValue* cmd = Get(o, "cmd");
  if (cmd == nullptr) {
    *err = "missing cmd";
    return false;
  }
  if (cmd->type != JsonType::kString || cmd->str != "sub") {
    *err = "unknown cmd";
    return false;
  }
  const JsonValue* sig = Get(o, "signal");
  if (sig == nullptr) {
    *err = "missing signal";
    return false;
  }
  if (sig->type != JsonType::kString || sig->str.empty()) {
    *err = "bad signal";
    return false;
  }
  const JsonValue* sym = Get(o, "symbol");
  if (sym == nullptr) {
    *err = "missing symbol";
    return false;
  }
  if (sym->type != JsonType::kString || sym->str.empty()) {
    *err = "bad symbol";
    return false;
  }
  out->signal = sig->str;
  out->symbol = sym->str;
  return true;
}

namespace {

bool ParseU64Field(const std::map<std::string, JsonValue>& o, const std::string& key,
                   std::uint64_t* out, std::string* err) {
  const JsonValue* v = Get(o, key);
  if (v == nullptr) {
    *err = "missing " + key;
    return false;
  }
  if (v->type != JsonType::kInt || !ParseU64(v->num, out)) {
    *err = "bad " + key;
    return false;
  }
  return true;
}

bool ParseI64Field(const std::map<std::string, JsonValue>& o, const std::string& key,
                   std::int64_t* out, std::string* err) {
  const JsonValue* v = Get(o, key);
  if (v == nullptr) {
    *err = "missing " + key;
    return false;
  }
  if (v->type != JsonType::kInt || !ParseI64(v->num, out)) {
    *err = "bad " + key;
    return false;
  }
  return true;
}

}  // namespace

bool ParseServerFrame(const std::string& line, ServerFrame* out, std::string* err) {
  if (line.size() > kMaxSignalLineLen) {
    *err = "line too long";
    return false;
  }
  std::map<std::string, JsonValue> o;
  if (!ParseObject(line, &o)) {
    *err = "malformed";
    return false;
  }
  const JsonValue* type = Get(o, "type");
  if (type == nullptr) {
    *err = "missing type";
    return false;
  }
  if (type->type != JsonType::kString) {
    *err = "bad type";
    return false;
  }
  const std::string& t = type->str;
  if (t == "sub_ack") {
    const JsonValue* ok = Get(o, "ok");
    if (ok == nullptr) {
      *err = "missing ok";
      return false;
    }
    if (ok->type != JsonType::kBool) {
      *err = "bad ok";
      return false;
    }
    const JsonValue* e = Get(o, "err");
    if (e == nullptr) {
      *err = "missing err";
      return false;
    }
    if (e->type != JsonType::kString) {
      *err = "bad err";
      return false;
    }
    out->type = ServerFrameType::kSubAck;
    out->ack.ok = ok->boolean;
    out->ack.err = e->str;
    return true;
  }
  if (t == "signal") {
    const JsonValue* sig = Get(o, "signal");
    if (sig == nullptr) {
      *err = "missing signal";
      return false;
    }
    if (sig->type != JsonType::kString || sig->str.empty()) {
      *err = "bad signal";
      return false;
    }
    const JsonValue* sym = Get(o, "symbol");
    if (sym == nullptr) {
      *err = "missing symbol";
      return false;
    }
    if (sym->type != JsonType::kString || sym->str.empty()) {
      *err = "bad symbol";
      return false;
    }
    const JsonValue* act = Get(o, "action");
    if (act == nullptr) {
      *err = "missing action";
      return false;
    }
    SignalAction action;
    if (act->type != JsonType::kString || !ParseSignalAction(act->str, &action)) {
      *err = "unknown action";
      return false;
    }
    std::uint64_t seq;
    if (!ParseU64Field(o, "seq", &seq, err)) return false;
    std::int64_t ts_us;
    if (!ParseI64Field(o, "ts_us", &ts_us, err)) return false;
    out->type = ServerFrameType::kSignal;
    out->push.signal = sig->str;
    out->push.symbol = sym->str;
    out->push.action = action;
    out->push.seq = seq;
    out->push.ts_us = ts_us;
    out->push.fields.clear();
    const JsonValue* f = Get(o, "fields");
    if (f != nullptr) {
      if (f->type != JsonType::kObject) {
        *err = "bad fields";
        return false;
      }
      out->push.fields = f->obj;
    }
    return true;
  }
  if (t == "hb") {
    std::uint64_t seq;
    if (!ParseU64Field(o, "seq", &seq, err)) return false;
    std::int64_t ts_us;
    if (!ParseI64Field(o, "ts_us", &ts_us, err)) return false;
    out->type = ServerFrameType::kHeartbeat;
    out->hb.seq = seq;
    out->hb.ts_us = ts_us;
    return true;
  }
  *err = "unknown type";
  return false;
}

}  // namespace kairos::exec
