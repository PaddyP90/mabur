#pragma once
// WebPhase: wraps maburplay's PhaseLock + SourceRate + RateMatch for the
// web GS core. The page (not a release thread) owns the vblank grid and
// calls on_frame() for every displayed frame and tick() at ~20 Hz; this
// class does the same decisions gs/player's housekeeping loop does
// (docs/phase-lock.md). It has no RCF wiring of its own: tick()'s return is
// what the page hands the core thread to put in the RCF.
#include <cstdint>

#include "phase_lock.h"
#include "rate_match.h"

namespace webgs {

class WebPhase {
 public:
  static constexpr double kTargetMs = 4.0;

  WebPhase() : lock_(kTargetMs) {}

  // Every displayed frame. margin_us is NaN when the page has no grid/
  // window yet; the pts still feeds the source-rate estimate. Margins are
  // ignored while mismatch() holds -- the lock is paused, not fed a number
  // measured against a panel the source cannot reach -- and while not
  // steering (the last tick() was not visible).
  void on_frame(uint32_t pts_us, double margin_us, double period_us, uint64_t now_us);
  // The pts timeline restarted (drone reboot): forget the centre, send 0.
  void on_discontinuity(uint64_t now_us);
  // ~20 Hz from the page. Returns the snr_off to send now. visible=false
  // (hidden tab, or the page's lock disabled) stops steering: 0 is sent and
  // the lock is frozen -- no margins, no windows -- since the drone sits at
  // its trim and an error measured there says nothing about the lock's own
  // offset; integrating it walks the centre off. The first visible tick
  // after that restarts the lock from Waiting (centre 0, offset 0).
  // SourceRate/RateMatch keep running throughout.
  int16_t tick(uint64_t now_us, double panel_hz, maburplay::RateMatch::LowPower lp, bool visible);

  maburplay::PhaseLock::State state() const { return lock_.state(); }
  bool locked() const { return lock_.locked() && !match_.mismatch(); }
  int16_t sent() const { return sent_; }
  double err_ms() const { return lock_.error_ms(); }
  double centre_mfps() const { return lock_.centre_mfps(); }
  bool mismatch() const { return match_.mismatch(); }
  double src_hz() const { return src_.hz(); }

 private:
  maburplay::PhaseLock lock_;
  maburplay::SourceRate src_;
  maburplay::RateMatch match_;
  bool steering_ = true;
  bool match_win_started_ = false;
  uint64_t match_win_start_us_ = 0;
  int16_t sent_ = 0;
};

// The core thread's view of the page's offset: 0 once the page has been
// silent for more than kStaleUs (a stalled or closed page must not leave
// the drone steered).
constexpr uint64_t kSnrOffStaleUs = 3'000'000;
int16_t snr_off_to_send(int16_t last, uint64_t set_us, uint64_t now_us);

}  // namespace webgs
