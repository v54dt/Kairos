#include "predicate_depth_evap.h"

#include <cmath>
#include <string>
#include <utility>

namespace kairos::exec {

namespace {

long SideVolume(const TopOfBook& tob, DepthSide side) {
  long total = 0;
  if (side == DepthSide::kBid) {
    for (int i = 0; i < tob.n_bids; ++i) total += tob.bids[i].volume;
  } else {
    for (int i = 0; i < tob.n_asks; ++i) total += tob.asks[i].volume;
  }
  return total;
}

}  // namespace

DepthEvapPredicate::DepthEvapPredicate(std::string name, std::vector<std::string> symbols,
                                       DepthEvapParams params)
    : Predicate(std::move(name), std::move(symbols)), params_(params) {}

std::optional<PredicateFire> DepthEvapPredicate::OnQuote(const std::string& symbol,
                                                         const TopOfBook& tob, std::int64_t ts_us) {
  SymbolState& st = state_[symbol];
  double vol = static_cast<double>(SideVolume(tob, params_.side));

  if (!st.have_baseline) {
    st.have_baseline = true;
    st.baseline = vol;
    st.first_ts = ts_us;
    st.last_ts = ts_us;
    return std::nullopt;
  }

  double dt_s = static_cast<double>(ts_us - st.last_ts) / 1e6;
  st.last_ts = ts_us;
  if (dt_s > 0.0 && params_.window_s > 0.0 && !st.fired) {
    double alpha = 1.0 - std::exp(-dt_s / params_.window_s);
    st.baseline += alpha * (vol - st.baseline);
  }

  if (st.baseline <= 0.0) return std::nullopt;
  double ratio = vol / st.baseline;
  bool warm = static_cast<double>(ts_us - st.first_ts) / 1e6 >= params_.warmup_s;

  if (!st.fired) {
    if (!warm || ratio >= params_.ratio_enter) return std::nullopt;
    if (st.have_enter && ts_us - st.last_enter_ts < params_.cooldown_us) return std::nullopt;
    st.fired = true;
    st.have_enter = true;
    st.last_enter_ts = ts_us;
    PredicateFire fire;
    fire.action = SignalAction::kEnter;
    fire.fields["ratio"] = std::to_string(ratio);
    return fire;
  }

  if (ratio > params_.ratio_exit) {
    st.fired = false;
    PredicateFire fire;
    fire.action = SignalAction::kExit;
    fire.fields["ratio"] = std::to_string(ratio);
    return fire;
  }
  return std::nullopt;
}

}  // namespace kairos::exec
