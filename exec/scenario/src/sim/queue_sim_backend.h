#ifndef KAIROS_EXEC_SIM_QUEUE_SIM_BACKEND_H_
#define KAIROS_EXEC_SIM_QUEUE_SIM_BACKEND_H_

#include "sim_order_backend.h"

namespace kairos::exec {

// In-process realistic paper backend (default PAPER mode). Fed the same
// TopOfBook/Trade stream the strategy sees, it fills through the queue model
// instead of PaperOrderBackend's instant full fill, so a passive/join strategy's
// true fill rate is visible in paper. It is also the extension point for shadow
// paper (running one alongside a live backend and reporting fill-rate divergence).
class QueueSimBackend : public SimOrderBackend {
 public:
  using SimOrderBackend::SimOrderBackend;

  // The queue model covers round-lot continuous/auction matching only, and the
  // fill engine rejects odd-lot submits. Odd-lot is the scenario default board, so
  // fill it instantly here (as instant paper does) instead of turning default paper
  // into a reject storm for every odd-lot scenario.
  void Submit(const OrderSubmitMsg& order) override {
    if (order.board == Board::kOddLot) {
      if (on_ack_) on_ack_(order.id, true, "");
      if (on_fill_) on_fill_(order.id, Fill{order.shares, order.price});
      return;
    }
    SimOrderBackend::Submit(order);
  }
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_QUEUE_SIM_BACKEND_H_
