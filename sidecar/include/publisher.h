#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Aeron.h"

namespace kairos {

class Publisher {
 public:
  Publisher(const std::string& aeron_dir, std::int32_t stream_id);
  bool offer(const std::vector<std::uint8_t>& payload);

 private:
  std::shared_ptr<aeron::Aeron> aeron_;
  std::shared_ptr<aeron::Publication> publication_;
};

}  // namespace kairos
