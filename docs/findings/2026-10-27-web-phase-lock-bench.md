# Does the built web GS phase lock hold on desktop Chrome, a Mac and a phone?

**Status:** bench of the built feature; the reference pages are
`docs/web-gs.md` ("Phase lock") and `docs/phase-lock.md` ("Web GS"). The
spike that led to it is `2026-10-27-web-phase-lock-spike.md`. The GS side
(maburplay/maburgs with the ±500 mfps range and the 0.55 Hz reach) was not
redeployed for this bench.

## Question

The branch `web-phase-lock` builds what the spike prototyped: maburplay's
`PhaseLock` and `RateMatch` in the WASM core, a fitted rAF grid, a frame
hold, a page toggle, a Stats group, and a ±500 mfps steering range on both
ends. Does it lock and hold in a real browser, do its exits work (toggle,
hidden tab, rate mismatch), and does the wider range let a phone panel lock?

## Rig

- **Drone:** 1920x1080@60 (mode 2), the branch's `maburd` (±500 mfps steer
  clamp). Boot trim request 59988 mfps. The GS was powered off, so the page
  was the only commander.
- **Page:** the branch's build served from `web/dist` with `web/serve.py`.
- **Host Chrome:** Chrome 147 on GNOME/Wayland, 3840x2160@60.000, headed,
  scratch profile with the WebUSB grant pre-written, driven over CDP. RTL8812EU.
- **Mac:** MacBook Air M4, macOS 26.6, Chrome; page through an ssh reverse
  tunnel as `http://localhost`, CDP through a forward tunnel; the same
  RTL8812EU on USB-C.
- **Phone:** vivo X200 Pro (V2413), Android 16, Chrome 155; page served
  through `adb reverse` as `http://localhost`, CDP through `adb forward`, all
  over wireless adb; the same RTL8812EU on OTG; the page in fullscreen
  landscape.
- **Measurement:** the page's Stats "Phase lock" rows sampled once a second
  over CDP (host). On the phone the fullscreen view has no Stats tab, so
  `PhaseController.snapshot()` was read once a second through a read-only
  CDP heap query. The drone's stats line (`snr_off=`) over ssh. A CDP trace
  for Chrome's pipeline depth on the host.

## Bench finding: the page crashed on load

The first load threw `ReferenceError: Cannot access ... before
initialization` from `getModule` and rendered nothing. `App.svelte`
constructed the pipeline before `session` existed, and the phase
controller's constructor called `getModule()` through `reset()`. The unit
tests cannot see `App.svelte`'s ordering. Fixed on the branch: no
constructor calls the module accessors, and `session` is declared first.

## Results — host Chrome

| check | result |
|---|---|
| first lock after Connect (channel scan included) | ~50 s |
| locked share, 120 s / 150 s runs | 84 % / 93 % |
| \|err\| while locked | p50 0.3 ms, max 3.2–4.5 ms |
| offset while locked | −110 … +92 mfps |
| late / displaced | 18 of 5944 and 0 of 8333; 5 and 2 |
| toggle off | drone `snr_off` 0 within ~2 s; Stats `state off` |
| toggle on | `lock` again within ~5 s |
| tab hidden 10 s | drone `snr_off` 0 within ~3 s; back in `lock` ~10 s after return; 60 fps, no decoder stall |
| drone at 1728x972@100 vs the 59.997 Hz panel | `drone 99.960 Hz · mismatch`, state `wait`, held count frozen (no holding), drone `snr_off` 0 |
| page capture→glass, 60 s window | p99 28.4 ms, max 43.9 ms |

The unlocks follow one pattern. The error drifts out 1.5–3.5 ms over several
seconds and comes back (three episodes in 150 s). That is the lateness
window's allowance stepping as late samples enter and age out, the same
mechanism behind maburplay's unlocks
(`2026-10-22-burned-dvr-100fps-throughput.md`). It is not the page's grid:
the rAF period stayed at 16.6675 ms. Chrome's own pipeline depth flapped
from 2 to 1 vsync during a 10 s trace (swap 10 → 2.5 ms, swap end →
presentation 22 → 13 ms), as in the spike.

## Results — phone

| check | result |
|---|---|
| locked share, 120 s | 96 % (115 of 120 s) |
| \|err\| while locked | p50 0.36 ms, max 2.6 ms |
| offset | +196 … +306 mfps, beyond the old ±200 clamp for the whole run |
| rates | panel 60.253 Hz, drone steered to 60.248 Hz |
| late / displaced | 43 of 7485 (0.57 %) / 1 |

- **The ±500 range did its job.** The drone's first steer logged
  `first offset 140 mfps`, and the offset then held around +250. The old
  range clamped at +200, below what this panel needs.
- **The phone changes refresh rate on its own.** After the tab switcher and
  leaving fullscreen, Chrome's rAF ran fast enough that the grid fitted
  103 Hz. The page flagged the mismatch and stopped steering, then went back
  to locking once the panel returned to 60 Hz. A 90 or 120 Hz phone panel
  against a 60 fps drone is a mismatch by design (the same rule as
  maburplay's `RateMatch`), so on such phones the lock only runs while the
  panel sits at 60 Hz.
- A tab Chrome restores after a force-close can run off-screen. It reports
  `visible` while rAF never fires, so the page gets no grid and does not
  steer, until the tab is brought to the front.

## Results — Mac

| check | result |
|---|---|
| on battery at 20 % | Chrome drew at 30 Hz (Energy Saver caps frame rates); the page fitted 30.000 Hz, flagged the mismatch and stopped steering — 182 frames displaced in the seconds before the flag (two drone frames per refresh while still holding) |
| on AC, 120 s | 83 % locked: every acquire in the first ~40 s after the 30 → 60 Hz switch, then 81 s locked straight; \|err\| p50 0.7 ms |
| on AC, next 60 s | 77 % locked, one 14 s re-acquire (the allowance-step pattern) |
| late / displaced | 5 of 5998 and 4 of 2706; 4 and 1 |
| grid under the rAF wobble | raw steps alternated 14.7 / 18.4 ms while frames were drawn in rAF; the fitted grid read 60.0000 Hz throughout |

The fitted grid holds the Mac's ±1.9 ms rAF wobble, which the raw-timestamp
grid in the spike did not (it displaced up to 25 % of frames there). Chrome's
battery Energy Saver halves the page's refresh, and the page treats that as
the rate mismatch it is.

## What this means

- The built lock holds on all three. The phone, the case the ±500
  range was added for, held 96 % of a 120 s run. Host Chrome held 84–93 %,
  limited by allowance steps, not by the browser.
- The exits behave: toggle, hidden tab and rate mismatch each return the
  drone to its trim within the 3 s budget.
- Two gates in the plan were missed narrowly. Host lock share in one of two
  runs was under 90 %, and the phone's late share was 0.57 % against 0.5 %.
  Both trace to the lateness window's allowance, the same lever maburplay
  has open.
- The Mac locks and holds with its rAF wobble. A laptop on low battery runs
  Chrome at 30 Hz and the lock stands down.
- Not run: the GS redeploy with the new range and reach.
