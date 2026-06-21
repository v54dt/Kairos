#include "publisher.h"

#include <chrono>
#include <thread>

#include "concurrent/AtomicBuffer.h"

namespace kairos::concords {

Publisher::Publisher(const std::string& aeron_dir, std::int32_t stream_id) {
  aeron::Context ctx;
  if (!aeron_dir.empty()) {
    ctx.aeronDir(aeron_dir);
  }
  aeron_ = aeron::Aeron::connect(ctx);
  std::int64_t reg = aeron_->addPublication("aeron:ipc", stream_id);
  while (!(publication_ = aeron_->findPublication(reg))) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

std::int64_t Publisher::Offer(const std::vector<std::uint8_t>& payload) {
  aeron::concurrent::AtomicBuffer buffer(const_cast<std::uint8_t*>(payload.data()), payload.size());
  std::int64_t r = 0;
  for (int attempt = 0; attempt < 5; ++attempt) {
    r = publication_->offer(buffer, 0, static_cast<std::int32_t>(payload.size()));
    if (r >= 0) return r;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return r;  // last negative Aeron code
}

}  // namespace kairos::concords
