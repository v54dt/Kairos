// Fail-closed codec test for the signal wire protocol. Valid frames roundtrip in
// both directions (including hostile strings); an inline reject corpus proves the
// oversized/malformed/unknown-type/unknown-action/missing-field/wrong-json-type
// paths all return false with a non-empty error. No cross-language golden file:
// no Rust/tui consumer of signals exists yet, so the corpus stays inline in C++.

#include <cstdint>
#include <string>
#include <vector>

#include "signal_proto.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

// Newline, tab, quote, backslash, a C0 control (0x1c), CJK -- same shape as the
// journal/ctl corpora.
const std::string kHostile = "reject:\nline2\ttab \"q\" \\slash \x1c sep \xe5\x8f\xb0\xe7\xa9\x8d";

void TestSubscribeRoundtrip() {
  SignalSubscribe in;
  in.signal = kHostile;
  in.symbol = "2330";
  std::string line = SerializeSubscribe(in);
  SignalSubscribe out;
  std::string err;
  CHECK(ParseSubscribe(line, &out, &err));
  CHECK(out.signal == in.signal);
  CHECK(out.symbol == in.symbol);
}

void TestAckRoundtrip() {
  for (bool ok : {true, false}) {
    SignalAck in;
    in.ok = ok;
    in.err = ok ? "" : kHostile;
    ServerFrame out;
    std::string err;
    CHECK(ParseServerFrame(SerializeAck(in), &out, &err));
    CHECK(out.type == ServerFrameType::kSubAck);
    CHECK(out.ack.ok == in.ok);
    CHECK(out.ack.err == in.err);
  }
}

void TestSignalRoundtrip() {
  SignalPush in;
  in.signal = kHostile;
  in.symbol = "TXFG5";
  in.action = SignalAction::kExit;
  in.seq = 18446744073709551615ULL;       // UINT64_MAX
  in.ts_us = -9223372036854775807LL - 1;  // INT64_MIN
  in.fields = {{"px", "58050"}, {"note", kHostile}, {"", "empty-key"}};
  ServerFrame out;
  std::string err;
  CHECK(ParseServerFrame(SerializeSignal(in), &out, &err));
  CHECK(out.type == ServerFrameType::kSignal);
  CHECK(out.push.signal == in.signal);
  CHECK(out.push.symbol == in.symbol);
  CHECK(out.push.action == SignalAction::kExit);
  CHECK(out.push.seq == in.seq);
  CHECK(out.push.ts_us == in.ts_us);
  CHECK(out.push.fields == in.fields);

  SignalPush no_fields;
  no_fields.signal = "s";
  no_fields.symbol = "y";
  no_fields.action = SignalAction::kEnter;
  no_fields.seq = 0;
  no_fields.ts_us = 0;
  CHECK(ParseServerFrame(SerializeSignal(no_fields), &out, &err));
  CHECK(out.push.fields.empty());
  CHECK(out.push.action == SignalAction::kEnter);
}

void TestHeartbeatRoundtrip() {
  SignalHeartbeat in;
  in.seq = 42;
  in.ts_us = 1700000000000000LL;
  ServerFrame out;
  std::string err;
  CHECK(ParseServerFrame(SerializeHeartbeat(in), &out, &err));
  CHECK(out.type == ServerFrameType::kHeartbeat);
  CHECK(out.hb.seq == in.seq);
  CHECK(out.hb.ts_us == in.ts_us);
}

void CheckRejectSub(const std::string& line) {
  SignalSubscribe out;
  std::string err;
  CHECK(!ParseSubscribe(line, &out, &err));
  CHECK(!err.empty());
}

void CheckRejectServer(const std::string& line) {
  ServerFrame out;
  std::string err;
  CHECK(!ParseServerFrame(line, &out, &err));
  CHECK(!err.empty());
}

void TestSubscribeRejects() {
  const std::string over = "{\"cmd\":\"sub\",\"signal\":\"" + std::string(4096, 'x') + "\"}";
  const std::vector<std::string> bad = {
      std::string(4097, 'x'),                                      // oversized
      over,                                                        // oversized (valid-ish shape)
      "",                                                          // empty
      "not json",                                                  // garbage
      "{",                                                         // unterminated
      "[]",                                                        // array, not object
      "{\"cmd\":\"sub\"}",                                         // missing signal
      "{\"cmd\":\"sub\",\"signal\":\"a\"}",                        // missing symbol
      "{\"signal\":\"a\",\"symbol\":\"b\"}",                       // missing cmd
      "{\"cmd\":\"nope\",\"signal\":\"a\",\"symbol\":\"b\"}",      // unknown cmd
      "{\"cmd\":1,\"signal\":\"a\",\"symbol\":\"b\"}",             // cmd wrong type
      "{\"cmd\":\"sub\",\"signal\":1,\"symbol\":\"b\"}",           // signal wrong type
      "{\"cmd\":\"sub\",\"signal\":\"\",\"symbol\":\"b\"}",        // signal empty
      "{\"cmd\":\"sub\",\"signal\":\"a\",\"symbol\":\"\"}",        // symbol empty
      "{\"cmd\":\"sub\",\"signal\":\"a\",\"symbol\":null}",        // null value
      "{\"cmd\":\"sub\",\"signal\":\"a\",\"symbol\":\"b\"} junk",  // trailing garbage
      "{\"cmd\":\"sub\",\"signal\":\"a\",\"symbol\":\"b\"",        // unterminated object
  };
  for (const std::string& b : bad) CheckRejectSub(b);
}

void TestServerRejects() {
  // Structural / type-token rejects. Each line is either malformed, an unknown
  // type/action, a missing required field, or a wrong JSON value type.
  const std::vector<std::string> bad = {
      std::string(4097, 'x'),
      "",
      "garbage",
      "{\"seq\":1,\"ts_us\":2}",
      "{\"type\":5}",
      "{\"type\":\"nope\"}",
      "{\"type\":\"sub_ack\",\"err\":\"x\"}",
      "{\"type\":\"sub_ack\",\"ok\":\"yes\",\"err\":\"x\"}",
      "{\"type\":\"sub_ack\",\"ok\":true}",
      "{\"type\":\"hb\",\"ts_us\":2}",
      "{\"type\":\"hb\",\"seq\":2}",
      "{\"type\":\"hb\",\"seq\":-1,\"ts_us\":2}",
      "{\"type\":\"hb\",\"seq\":\"2\",\"ts_us\":2}",
      "{\"type\":\"hb\",\"seq\":2,\"ts_us\":1.5}",
      "{\"type\":\"hb\",\"seq\":18446744073709551616,\"ts_us\":0}",
      "{\"type\":\"hb\",\"seq\":01,\"ts_us\":0}",
      "{\"type\":\"signal\",\"symbol\":\"y\",\"action\":\"enter\",\"seq\":1,\"ts_us\":2}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"action\":\"enter\",\"seq\":1,\"ts_us\":2}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"symbol\":\"y\",\"seq\":1,\"ts_us\":2}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"symbol\":\"y\",\"action\":\"hold\",\"seq\":1,\"ts_"
      "us\":2}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"symbol\":\"y\",\"action\":\"enter\",\"ts_us\":2}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"symbol\":\"y\",\"action\":\"enter\",\"seq\":1}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"symbol\":\"y\",\"action\":1,\"seq\":1,\"ts_us\":2}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"symbol\":\"y\",\"action\":\"enter\",\"seq\":1,\"ts_"
      "us\":2,\"fields\":\"x\"}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"symbol\":\"y\",\"action\":\"enter\",\"seq\":1,\"ts_"
      "us\":2,\"fields\":{\"k\":1}}",
      "{\"type\":\"signal\",\"signal\":\"s\",\"symbol\":\"y\",\"action\":\"enter\",\"seq\":1,\"ts_"
      "us\":2,\"fields\":{\"k\":{\"n\":\"v\"}}}",
  };
  for (const std::string& b : bad) CheckRejectServer(b);
}

// A subscribe frame must not decode as a server frame and vice versa.
void TestCrossDirectionRejects() {
  SignalSubscribe sub;
  sub.signal = "s";
  sub.symbol = "y";
  CheckRejectServer(SerializeSubscribe(sub));  // "sub" has no server type

  SignalHeartbeat hb;
  hb.seq = 1;
  CheckRejectSub(SerializeHeartbeat(hb));  // "hb" has no cmd
}

}  // namespace

int main() {
  TestSubscribeRoundtrip();
  TestAckRoundtrip();
  TestSignalRoundtrip();
  TestHeartbeatRoundtrip();
  TestSubscribeRejects();
  TestServerRejects();
  TestCrossDirectionRejects();
  if (g_failures == 0) {
    std::printf("test_signal_proto: OK\n");
    return 0;
  }
  std::printf("test_signal_proto: FAILED %d check(s)\n", g_failures);
  return 1;
}
