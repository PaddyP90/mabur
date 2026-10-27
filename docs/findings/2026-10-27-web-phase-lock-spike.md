# Can the web GS phase-lock the drone's sensor to the browser's refresh?

**Status:** spike; nothing built. The web GS still sends `snr_off` 0
(`docs/phase-lock.md` "Wire"; the page itself is `docs/web-gs.md`). The
throwaway code is three commits on a local branch, `web-phase-lock-spike`,
never pushed, and is described below well enough to rebuild. Two runs the
same day: desktop Chrome on the bench host (first half) and Chrome on an
Android phone (second half, "Phone").

## Question

maburplay locks the drone's capture phase to the GS vblank by steering the
sensor rate (`PhaseLock`, `docs/phase-lock.md`). The web GS draws each
decoded frame straight to a canvas and has no regulator. Can a page see a
vblank grid steady enough to run the same loop, does the lock hold, and
does it make the time to the screen flat as it does on the Radxa?

## Rig

- **Core**: `gs/player/src/phase_lock.cpp` compiled unchanged into the WASM
  core (`webgs_core`). Page-thread exports (`webgs_pl_reset/margin/tick/
  force/...`) drive a global `PhaseLock`. Its offset goes into an atomic
  that the live loop hands to `VrxController::set_snr_off` every
  iteration, so every RCF carries it. `webgs_pl_force` pins the offset
  open-loop (the "unlocked" rows send 0).
- **Page** (`phase_spike.js`):
  - **Vsync grid.** A continuous `requestAnimationFrame` loop. The period
    is a fit over the last 240 steps, skipping missed frames; the anchor is
    the latest rAF timestamp.
  - **Margin.** On each decoder output the page computes
    `want = pts + floor + allowance`, takes the first grid tick at or after
    it, and feeds `margin = tick − want` with the period to `PhaseLock`.
    Ticks run every 50 ms. Everything is on `performance.now()`, the rAF
    clock.
  - **Floor and allowance.** The first version took them per 1 s block (p1,
    and p99 − p1 + 1 capped at 16 ms). The final version uses maburplay's
    `LatenessWindow` instead: a rolling 600-frame p1/p99. The desktop rows
    used the first version, the phone rows from "rolling window" on the
    second.
- **Drone**: 1920x1080@60, boot trim request 59988 mfps, on the bench link.
  For the phone half, `kSnrOffRangeMfps` (the GS side) and the drone's
  `SENSOR_STEER_RANGE_MFPS` were raised from ±200 to ±500 mfps on the spike
  branch. The drone ran that `maburd` for the phone runs only and was
  restored afterwards.
- **GS**: powered off, so the page was the only commander.
- **Desktop**: GNOME on Wayland, mode 3840x2160@60.000. Chrome 147, headed,
  a scratch profile with the WebUSB grant written into its Preferences,
  driven over CDP. RTL8812EU card. Colortrans on (the WebGL canvas path).
- **Phone**: vivo X200 Pro (V2413), Android 16, Chrome 155. Panel modes
  60/72/90/120 Hz. The same RTL8812EU card on OTG. The page was served
  through `adb reverse` as `http://localhost` (a secure context without a
  trusted certificate) and driven over CDP through `adb forward` to
  `chrome_devtools_remote`, all over wireless adb.
- **Measurement.** Per frame, the page logs the transit `tOut − pts`, the
  margin, and `pd` = the next rAF timestamp − `tOut`. That is how long the
  frame waits for the frame that picks it up; unlocked, it is the beat
  sawtooth. Real screen time comes from a CDP trace (`cc`, `benchmark`,
  `viz`, `gpu`, `devtools.timeline.frame`):
  - **Presentation.** A `PipelineReporter` async slice begins at the
    BeginFrame's frame time and ends at the presentation timestamp.
  - **Matching.** A decoded frame belongs to the first
    `ProxyMain::BeginMainFrame` that runs on the renderer main thread after
    its `tOut`. That event's `begin_frame_id` names the reporter.
  - **Clock sync.** Three `performance.mark`s map the page clock onto trace
    time (spread ≤ 19 µs).

## Results — desktop

**Grid.** The rAF period was 16.6675 ms (59.997 Hz against the mode's
nominal 60.000); its step jitter (p90 − p10) was 0.005 ms. The drone sat
9 mfps away.

**Lock.** It locked about 25 s after Connect, channel scan included. From
oor (after the offset had been pinned) it re-locked in under 40 s. The
drone's stats `snr_off` followed the page's offset (−40 to +29 mfps).

| 150 s run | margin p10/p50/p90 | `pd` p10/p50/p90 | `pd` 10 s medians |
|---|---|---|---|
| locked (target 4) | 3.16 / 3.92 / 4.92 | 6.43 / 8.34 / 9.21 | 7.7–8.7, flat |
| unlocked (`snr_off` 0) | 2.12 / 10.38 / 15.21 | 0.93 / 6.81 / 14.97 | 8.1 → 0.75, wraps to 15.25 → 11.0 |

Unlocked, the wait falls ~0.1 ms/s, a ~160 s beat.

**Trace, decode output → presentation (ms).**

| trace | pipeline depth | p1 / p10 / p50 / p90 / p99 | wait to its BeginFrame p1 / p50 / p99 |
|---|---|---|---|
| locked, target 4 | 4 vsyncs (some 5) | 71.3 / 73.9 / 74.6 / 75.6 / 91.5 | −9.25 / 7.83 / 9.28 |
| locked, target 4, later | 2 vsyncs | 37.7 / 40.3 / 41.4 / 42.1 / 42.5 | 4.36 / 8.02 / 9.13 |
| locked, target 1 | 1 vsync (p99 2) | 18.2 / 20.9 / 21.9 / 23.8 / 39.1 | 1.45 / 5.03 / 6.60 |
| forced +200 mfps sweep | 4–5, then 1 | 16.9 / 20.8 / 76.9 / 87.4 / 93.5 | −6.87 / 5.32 / 15.9 |

- **Locked, the time to the screen is flat.** p10–p90 spans 1.7–1.8 ms
  whatever the depth. Target 1 instead of 4 cut the wait by 3 ms (p50
  8.0 → 5.0) with no frame missing its BeginFrame (p1 1.45 ms).
- **Chrome's own pipeline depth is not stable**, and that swamps the lock.
  BeginFrame → presentation was 4–5 vsyncs, 2, or 1 at different times in
  one session. In the deep regime `Swap` blocks for ~33 ms and swap end →
  presentation takes ~23 ms; in the shallow one they take 2.6 and 13 ms.
  It switched from deep to shallow mid-trace, unprompted. A 16–66 ms swing
  is far larger than the ±8 ms beat the lock removes. Not investigated
  further: XWayland, a 1080p desktop and the flat-canvas path were not
  tried.
- **The deadline is when BeginMainFrame runs, not the frame time.** In the
  deep regime a frame drawn up to 9 ms after the frame time still made
  that frame, because BeginMainFrame ran late.

## Results — phone

**Grid.** Chrome's grid was not the panel's reported mode. SurfaceFlinger
showed the 90 Hz mode active throughout. rAF ticked a uniform 16.596 ms
(60.26 Hz, step jitter 0.03–0.06 ms), and the trace's frame times stepped
16.6 ms. BeginFrame → presentation was a constant 27.7 ms (p1 27.43, p99
27.77): a fixed, shallow pipeline. The transit `tOut − pts` did not drift,
so the phone's clock and the drone's agree and 60.26 Hz is real.

**Range.** Matching 60.26 Hz from the drone needs +205 to +305 mfps. With
the stock ±200 mfps range the centre pinned at +200 and the lock went oor.
All rows below ran on the ±500 build.

**Per-second blocks vs the rolling window.** With per-second floor and
allowance the lock held 30 of 90 s and the error swung ±6 ms. The per-block
p99 − p1 jumped between 4 and 40 ms (capped at 16), stepping `want`, and so
every margin, by several ms once a second. The rolling 600-frame window
fixed it.

| run (target 4) | state | margin p10/p50/p90 | `pd` p1 / p10 / p50 / p90 / p99 |
|---|---|---|---|
| per-second blocks, 90 s | locked 30/90 s | 2.45 / 5.05 / 15.0 | −0.53 / 4.46 / 8.13 / 11.2 / 14.3 |
| rolling window, 90 s | locked 90/90 s, \|err\| ≤ 1.83 ms | 3.36 / 3.97 / 4.67 | 4.60 / 7.44 / 9.07 / 11.2 / 13.8 |
| rolling window, 60 s | locked | 3.56 / 4.39 / 5.22 | 3.76 / 7.97 / 10.0 / 12.5 / 14.6 |
| unlocked (`snr_off` 0), 60 s | — | 1.75 / 8.27 / 14.9 | −1.28 / −0.20 / 8.42 / 13.7 / 15.0 |

**Tracing perturbs the phone.** Under the trace the delivery tail grew, the
allowance with it, and frames were held longer: the wait split at ~−0.5 ms
and 12–15 ms around the deadline, with BeginMainFrame running 0.94 ms (p50)
after the frame time. In both traces the page's `pd` equalled the traced
wait to 0.1 ms frame for frame, and BeginFrame → presentation was the
constant 27.7 ms. So on the phone, untraced `pd` + 27.7 ms is the time to
the screen, and the rows above are that measure.

**Cost.** The phone's delivery jitter is wider than the desktop's (allowance
7–10 ms against ~5 ms; transit p10–p90 1.5–5.0 against 0.8–3.5 ms). At
target 4 the lock adds about 1.6 ms of mean wait (`pd` p50 10.0 against
8.4 unlocked). What it buys is the beat: unlocked, at 0.27 Hz apart, a
frame is repeated every few seconds. A lower target, as on the desktop,
should win the mean back; it was not tried on the phone.

## What this means

- **The loop ports.** A page sees a refresh grid steady enough to run
  `PhaseLock` unchanged, the lock holds, and on the real screen it is flat
  on both platforms.
- **Two things the shipped design would need:**
  - A wider steering range. One phone's browser grid sat 0.27 Hz off
    nominal, beyond ±200 mfps. The desktop was 0.003 Hz off.
  - The rolling lateness window, not per-second estimates.
- **The target margin should be lower than maburplay's 4 ms.** The page
  draws at decode output with no lead to cover.
- **On this desktop the browser's own pipeline depth, not the beat,
  dominates the time to the screen.** A web phase lock is worth more on a
  platform whose pipeline is fixed, like this phone, than it would be
  here.
