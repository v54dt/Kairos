#ifndef KAIROS_EXEC_SIGNAL_PROTO_H_
#define KAIROS_EXEC_SIGNAL_PROTO_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace kairos::exec {

// Longest signal line the codec will parse; anything longer is rejected rather
// than buffered, so a malformed peer cannot grow memory without bound.
constexpr std::size_t kMaxSignalLineLen = 4096;

// The side a signal asks a subscriber to take. Fail-closed: there is no default;
// an absent or unrecognized token is rejected.
enum class SignalAction { kEnter, kExit };

// Parse the exact action token. Only "enter"/"exit" are accepted; anything else
// (missing, wrong case, garbage) returns false.
bool ParseSignalAction(const std::string& token, SignalAction* out);

const char* SignalActionName(SignalAction action);

// client -> daemon: {"cmd":"sub","signal":"<name>","symbol":"<sym>"}
struct SignalSubscribe {
  std::string signal;
  std::string symbol;
};

// daemon -> client: {"type":"sub_ack","ok":true|false,"err":"..."}
struct SignalAck {
  bool ok = false;
  std::string err;
};

// daemon -> client signal push. `fields` is an optional flat string map.
struct SignalPush {
  std::string signal;
  std::string symbol;
  SignalAction action = SignalAction::kEnter;
  std::uint64_t seq = 0;
  std::int64_t ts_us = 0;
  std::map<std::string, std::string> fields;
};

// daemon -> client: {"type":"hb","seq":<u64>,"ts_us":<i64>}
struct SignalHeartbeat {
  std::uint64_t seq = 0;
  std::int64_t ts_us = 0;
};

// The daemon->client frames share one wire stream; the client dispatches on the
// "type" token, so a decode yields a tagged union of the three server frames.
enum class ServerFrameType { kSubAck, kSignal, kHeartbeat };

struct ServerFrame {
  ServerFrameType type = ServerFrameType::kHeartbeat;
  SignalAck ack;
  SignalPush push;
  SignalHeartbeat hb;
};

// Each Serialize* returns one JSON line terminated by '\n'.
std::string SerializeSubscribe(const SignalSubscribe& sub);
std::string SerializeAck(const SignalAck& ack);
std::string SerializeSignal(const SignalPush& push);
std::string SerializeHeartbeat(const SignalHeartbeat& hb);

// Decode one client->daemon subscribe line. Returns true and fills *out on a
// valid request; returns false and fills *err on any malformed / unknown /
// fail-closed input (oversized line, unknown cmd, missing/bad field). Never
// throws.
bool ParseSubscribe(const std::string& line, SignalSubscribe* out, std::string* err);

// Decode one daemon->client frame, dispatching on "type". Same fail-closed
// contract as ParseSubscribe. Never throws.
bool ParseServerFrame(const std::string& line, ServerFrame* out, std::string* err);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIGNAL_PROTO_H_
