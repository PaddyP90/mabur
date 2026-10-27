// Host test for web/src/web_phase.h: the lock + rate-match wrapper the
// page drives directly (no RCF/UDP wiring here).
#include <cmath>
#include <cstdint>

#include "mtest.h"
#include "phase_lock.h"
#include "rate_match.h"
#include "web_phase.h"

using maburplay::PhaseLock;
using maburplay::RateMatch;
using webgs::WebPhase;

namespace {

// Feeds on_frame() every pts_step_us and tick() every 50 ms for duration_us,
// starting at *now (advanced in place). Returns the last tick()'s result.
int16_t run(WebPhase& wp, uint64_t* now, double margin_us, double panel_hz,
            RateMatch::LowPower lp, bool visible, uint64_t duration_us,
            uint32_t pts_step_us = 16'667, double period_us = 16'667.0) {
  uint32_t pts = 0;
  int16_t last = 0;
  uint64_t next_tick = *now + 50'000;
  const uint64_t end = *now + duration_us;
  while (*now < end) {
    wp.on_frame(pts, margin_us, period_us, *now);
    pts += pts_step_us;
    *now += pts_step_us;
    while (next_tick <= *now) {
      last = wp.tick(next_tick, panel_hz, lp, visible);
      next_tick += 50'000;
    }
  }
  return last;
}

}  // namespace

TEST(locks_on_target_margins) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  const int16_t last =
      run(wp, &now, /*margin_us=*/4'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
          /*visible=*/true, /*duration_us=*/6'000'000);
  CHECK(wp.state() == PhaseLock::State::Locked);
  CHECK(last == 0);
  CHECK(wp.sent() == 0);
  CHECK(wp.locked());
}

TEST(error_steers_within_acquire_clamp) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  // +2 ms error (margin 6000 vs the 4000 target) until the first window
  // closes: centre stays 0 (constant error -> zero slope), so the sent
  // offset is 0 - clamp(30*2, +-60) = -60.
  run(wp, &now, /*margin_us=*/6'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/1'100'000);
  CHECK(wp.state() == PhaseLock::State::Acquiring);
  CHECK(-60 <= wp.sent() && wp.sent() < 0);
}

TEST(hidden_sends_zero) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  run(wp, &now, /*margin_us=*/6'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/1'100'000);
  REQUIRE(wp.state() == PhaseLock::State::Acquiring);
  REQUIRE(wp.sent() < 0);
  const int16_t r = wp.tick(now + 50'000, 60.0, RateMatch::LowPower::kNo, /*visible=*/false);
  CHECK(r == 0);
  CHECK(wp.sent() == 0);
}

TEST(mismatch_sends_zero) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  // 100 fps source against a 60 Hz panel: a NaN margin stands in for "no
  // grid yet" so only the source-rate estimate (and the mismatch check) is
  // exercised, same as a page with margins still fed meanwhile.
  run(wp, &now, /*margin_us=*/4'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/4'000'000, /*pts_step_us=*/10'000);
  REQUIRE(wp.mismatch());
  const double err_before = wp.err_ms();
  const int16_t r = wp.tick(now + 50'000, 60.0, RateMatch::LowPower::kNo, /*visible=*/true);
  CHECK(r == 0);
  CHECK(wp.sent() == 0);
  // More margins fed while mismatched must not move the held error.
  wp.on_frame(0, 9'000.0, 16'667.0, now + 60'000);
  CHECK(wp.err_ms() == err_before);
}

TEST(low_power_is_never_a_mismatch) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  run(wp, &now, /*margin_us=*/4'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kYes,
      /*visible=*/true, /*duration_us=*/5'000'000, /*pts_step_us=*/33'333);
  CHECK(!wp.mismatch());
}

TEST(discontinuity_resets) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  run(wp, &now, /*margin_us=*/4'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/6'000'000);
  REQUIRE(wp.locked());
  wp.on_discontinuity(now);
  CHECK(wp.state() == PhaseLock::State::Waiting);
  const int16_t r = wp.tick(now + 50'000, 60.0, RateMatch::LowPower::kNo, /*visible=*/true);
  CHECK(r == 0);
}

TEST(stale_page_offset_falls_to_0) {
  CHECK(webgs::snr_off_to_send(-60, 1'000'000, 3'900'000) == -60);
  CHECK(webgs::snr_off_to_send(-60, 1'000'000, 4'100'000) == 0);
}

MTEST_MAIN
