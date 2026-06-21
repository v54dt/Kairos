#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Aeron.h"

namespace kairos::concords {

class Publisher {
 public:
  Publisher(const std::string& aeron_dir, std::int32_t stream_id);
  // Aeron offer result: >=0 published, <0 dropped (NOT_CONNECTED / BACK_PRESSURED
  // / ADMIN_ACTION / PUBLICATION_CLOSED).
  std::int64_t Offer(const std::vector<std::uint8_t>& payload);

 private:
  std::shared_ptr<aeron::Aeron> aeron_;
  std::shared_ptr<aeron::Publication> publication_;
};

}  // namespace kairos::concords
