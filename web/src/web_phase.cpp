#include "web_phase.h"

#include <cmath>

namespace webgs {

void WebPhase::on_frame(uint32_t pts_us, double margin_us, double period_us, uint64_t now_us) {
  src_.on_pts(pts_us, now_us);
  if (!steering_ || match_.mismatch() || std::isnan(margin_us)) return;
  lock_.on_margin(margin_us, period_us, now_us);
}

void WebPhase::on_discontinuity(uint64_t now_us) { lock_.on_discontinuity(now_us); }

int16_t WebPhase::tick(uint64_t now_us, double panel_hz, maburplay::RateMatch::LowPower lp,
                        bool visible) {
  if (visible && !steering_) lock_.on_discontinuity(now_us);
  steering_ = visible;
  if (steering_) lock_.tick(now_us);
  if (!match_win_started_) {
    match_win_started_ = true;
    match_win_start_us_ = now_us;
  } else if (now_us - match_win_start_us_ >= maburplay::PhaseLock::kWindowUs) {
    match_.on_window(src_.hz(), panel_hz, lp);
    match_win_start_us_ = now_us;
  }
  if (!visible || match_.mismatch()) {
    sent_ = 0;
    return sent_;
  }
  sent_ = static_cast<int16_t>(lock_.offset_mfps());
  return sent_;
}

int16_t snr_off_to_send(int16_t last, uint64_t set_us, uint64_t now_us) {
  return now_us - set_us > kSnrOffStaleUs ? 0 : last;
}

}  // namespace webgs
