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
  // 100 fps source against a 60 Hz panel, margins held at the target: the
  // lock would otherwise be locking fine, but RateMatch flags the mismatch
  // after 3 bad windows (~4 s).
  run(wp, &now, /*margin_us=*/4'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/4'000'000, /*pts_step_us=*/10'000);
  REQUIRE(wp.mismatch());
  const double err_before = wp.err_ms();
  const int16_t r = wp.tick(now + 50'000, 60.0, RateMatch::LowPower::kNo, /*visible=*/true);
  CHECK(r == 0);
  CHECK(wp.sent() == 0);
  now += 50'000;
  // Keep feeding a badly off-target margin (9000 us) for another full
  // window while still mismatched: if on_frame still passed it to
  // PhaseLock, the window that closes during this run would move
  // err_ms() off err_before. It must not, since margins are ignored while
  // mismatch() holds.
  run(wp, &now, /*margin_us=*/9'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/1'100'000, /*pts_step_us=*/10'000);
  CHECK(wp.mismatch());
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

// Not steering (tab hidden, or the lock disabled -- the page passes
// visible=false for both): the drone sits at 0, so the lock must not keep
// integrating margins measured against an offset that never reached it.
TEST(hidden_freezes_the_lock) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  run(wp, &now, /*margin_us=*/6'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/1'100'000);
  REQUIRE(wp.state() == PhaseLock::State::Acquiring);
  const double centre_before = wp.centre_mfps();
  const double err_before = wp.err_ms();
  // 10 s hidden with a badly off-target margin: every window that would
  // have closed here would move err_ms() (and the centre with it).
  run(wp, &now, /*margin_us=*/9'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/false, /*duration_us=*/10'000'000);
  CHECK(wp.centre_mfps() == centre_before);
  CHECK(wp.err_ms() == err_before);
}

TEST(visible_again_restarts_from_waiting) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  run(wp, &now, /*margin_us=*/6'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/1'100'000);
  REQUIRE(wp.state() == PhaseLock::State::Acquiring);
  REQUIRE(wp.sent() < 0);
  run(wp, &now, /*margin_us=*/9'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/false, /*duration_us=*/10'000'000);
  const int16_t r = wp.tick(now + 50'000, 60.0, RateMatch::LowPower::kNo, /*visible=*/true);
  CHECK(wp.state() == PhaseLock::State::Waiting);
  CHECK(r == 0);
  CHECK(wp.centre_mfps() == 0.0);
}

TEST(hidden_never_sends_nonzero) {
  WebPhase wp;
  uint64_t now = 1'000'000;
  run(wp, &now, /*margin_us=*/6'000.0, /*panel_hz=*/60.0, RateMatch::LowPower::kNo,
      /*visible=*/true, /*duration_us=*/1'100'000);
  REQUIRE(wp.sent() < 0);
  uint32_t pts = 0;
  uint64_t next_tick = now + 50'000;
  const uint64_t end = now + 10'000'000;
  int nonzero = 0;
  while (now < end) {
    wp.on_frame(pts, 9'000.0, 16'667.0, now);
    pts += 16'667;
    now += 16'667;
    while (next_tick <= now) {
      if (wp.tick(next_tick, 60.0, RateMatch::LowPower::kNo, /*visible=*/false) != 0) ++nonzero;
      next_tick += 50'000;
    }
  }
  CHECK(nonzero == 0);
  CHECK(wp.sent() == 0);
}

TEST(stale_page_offset_falls_to_0) {
  CHECK(webgs::snr_off_to_send(-60, 1'000'000, 3'900'000) == -60);
  CHECK(webgs::snr_off_to_send(-60, 1'000'000, 4'100'000) == 0);
}

MTEST_MAIN
