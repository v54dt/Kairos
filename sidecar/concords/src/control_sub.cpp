#include "control_sub.h"

#include <chrono>
#include <thread>
#include <utility>

#include "concurrent/AtomicBuffer.h"
#include "quote_encode.h"

namespace kairos::concords {

ControlSub::ControlSub(const std::string& aeron_dir, std::int32_t stream_id) {
  aeron::Context ctx;
  if (!aeron_dir.empty()) {
    ctx.aeronDir(aeron_dir);
  }
  aeron_ = aeron::Aeron::connect(ctx);
  std::int64_t reg = aeron_->addSubscription("aeron:ipc", stream_id);
  while (!(subscription_ = aeron_->findSubscription(reg))) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool ControlSub::Poll(std::vector<std::string>* out) {
  bool got = false;
  std::vector<std::string> latest;
  auto handler = [&](aeron::concurrent::AtomicBuffer& buffer, aeron::util::index_t offset,
                     aeron::util::index_t length, auto&) {
    std::vector<std::string> syms;
    if (DecodeSubscribe(buffer.buffer() + offset, static_cast<std::size_t>(length), &syms)) {
      latest = std::move(syms);
      got = true;
    }
  };
  subscription_->poll(handler, 10);
  if (got) {
    *out = std::move(latest);
  }
  return got;
}

}  // namespace kairos::concords
