#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Aeron.h"

namespace kairos::concords {

// Subscribes to the reverse control stream (core -> sidecar) and decodes the
// desired subscription set (capnp Envelope.subscribe, replace semantics).
class ControlSub {
 public:
  ControlSub(const std::string& aeron_dir, std::int32_t stream_id);

  // Poll the control stream once. If a fresh desired set arrived, write it to
  // *out and return true; otherwise return false.
  bool Poll(std::vector<std::string>* out);

 private:
  std::shared_ptr<aeron::Aeron> aeron_;
  std::shared_ptr<aeron::Subscription> subscription_;
};

}  // namespace kairos::concords
