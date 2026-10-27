# Can the web GS phase-lock the drone's sensor to the browser's refresh?

**Status:** spike; its servo was throwaway. The web GS now steers in
earnest: `docs/web-gs.md` ("Phase lock") and `docs/phase-lock.md` ("Web
GS"). The throwaway code described below (eight commits on a local branch,
`web-phase-lock-spike`, never pushed) was rebuilt as `web/ui/src/lib/phase.js`
rather than reused verbatim. Three runs the
same day: desktop Chrome on the bench host, Chrome on an Android phone
("Phone"), and Chrome on a MacBook Air ("Mac"). The Mac run also tried a
page-side hold and measured absolute capture → presentation ("Absolute
latency"). Every "decode → present" figure in this record is one segment
of the latency, not end to end.

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
    second, and the Mac rows the second.
  - **Hold** (Mac only, toggled at runtime). Without it, a frame is drawn
    at decode output. With it, the frame is queued and drawn inside the
    `requestAnimationFrame` callback of its release tick, which runs in
    that frame's BeginMainFrame, so the draw still commits with that frame.
    Older frames due at the same tick are closed ("displaced"). A frame
    whose release tick's rAF has already run is drawn at once ("late").
  - **Fitted grid** (Mac, after the hold showed the need). The anchor is
    the median phase of the last 240 rAF timestamps against the fitted
    period, snapped to the nearest tick, instead of the latest raw
    timestamp.
  - **Synthetic jitter** (Mac A/B only). A knob delays each decoded frame by
    U(0, 10) ms before the page sees it. At 60 fps this never reorders
    frames.
  - **Capture stamp** (Mac, absolute latency only). Each frame's capture
    time on the page clock is the core's emit stamp minus its
    `cap_to_complete_us`. That is the frame pts mapped onto GS time through
    the RTT offset, which is built from `MI_SYS_GetCurPts`, the frame pts's
    own timebase.
- **Drone**: 1920x1080@60, boot trim request 59988 mfps, on the bench link.
  For the phone half, `kSnrOffRangeMfps` (the GS side) and the drone's
  `SENSOR_STEER_RANGE_MFPS` were raised from ±200 to ±500 mfps on the spike
  branch. The drone ran that `maburd` for the phone runs only and was
  restored afterwards.
- **GS**: powered off, so the page was the only commander.
- **Desktop**: GNOME on Wayland, mode 3840x2160@60.000. Chrome 147, headed,
  a scratch profile with the WebUSB grant written into its Preferences,
  driven over CDP. RTL8812EU card. Colortrans on (the WebGL canvas path).
- **Mac**: MacBook Air M4, macOS 26.6, Chrome, built-in 2560x1664 panel
  (60 Hz). The same card on USB-C. The page was served from the bench host
  through an ssh reverse tunnel as `http://localhost`. Chrome was started
  over ssh in the logged-in session with a scratch profile (WebUSB grant
  pre-written) and driven over CDP through a forward tunnel.
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
    time (spread ≤ 32 µs).
  - **Held frames.** With the hold, a frame is matched by its draw time
    instead: the BeginMainFrame it was drawn inside, or the next one.
  - **Capture-referenced spread.** With a hold, decode → present varies by
    design (an early frame is held longer). What should be flat is capture
    → present, so the Mac rows use pts-referenced capture → present,
    relative to its own minimum, and count frames shown one whole refresh
    early or late ("off-slot").

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

## Results — Mac

**Grid.** rAF ran at 16.668 ms (59.995 Hz), within the stock ±200 mfps of
the drone, with step jitter ~0.2 ms. BeginFrame → presentation was 23–25 ms
(two values, ~23.0 and ~24.9), with no depth changes.

**Lock without the hold.** It locked within 30 s of Connect and held 72 of
90 s on a noisy stretch of link. A 91 ms stall was followed by tens of
seconds of raised transit (median 2 → 6–11 ms). The allowance sat at
13–16 ms, and both unlocks followed an allowance step. In that state a
typical frame is drawn 10+ ms before its `want`, so it lands at the
previous refresh's deadline. Locked, the wait to the frame's BeginFrame was
p10/p50/p90 1.4 / 12.1 / 15.1 ms, the median flat but the spread the whole
period. Unlocked it was 1.3 / 7.3 / 13.7 with 10 s medians sweeping a
~150 s beat. The lock fixes the mean phase, but a page that draws at
decode output spreads frames across the period once jitter approaches it.

**The hold.** Traces, capture → present relative to its minimum:

| run (target 4 unless noted) | off-slot | in-slot spread p1–p99 |
|---|---|---|
| no hold, noisy link (above) | 39.7 % (mostly one early) | 3.4 ms |
| no hold, calm link | 0.6 % | 0.96 ms |
| hold, calm link | 0.8 % | 0.55 ms |
| no hold + U(0,10) jitter | 10.6 % (59 early, 7 late of 620) | 0.84 ms |
| hold + U(0,10) jitter | 3.6 % (1 early, 22 late of 634) | 1.71 ms |
| hold, fitted grid, calm | ~1 % (0 displaced, 4 late in 60 s) | 1.7 ms (p1–p90) |

- **On a calm link the hold changes nothing.** Every frame makes its slot
  either way, at the same latency (wait to BeginFrame p50 10.7 held, 11.1
  not).
- **Under jitter it keeps the cadence.** Without a hold, frames that
  arrive early jump a refresh ahead, which is judder. With it, the
  remaining strays are frames later than the allowance, which sat at its
  16 ms cap under the synthetic jitter.
- **The hold needs slack.** At target 1 the slot each frame maps to flips
  between neighbouring ticks with millisecond `want` jitter (pts spacing
  and allowance changes), and two frames land on one tick: 25 % displaced
  on the raw-timestamp grid, still 4.4 % on the fitted grid. Target 4: 0
  displaced. The 3 ms the desktop saved at target 1 without a hold is not
  available with one; maburplay's 4 ms margin is the slack.
- **Drawing in rAF made Chrome's rAF timestamps wobble.** With the hold on,
  steps alternated 14.8 / 18.6 ms (step jitter 0.4 → 3.8 ms) though
  presentations stayed on the grid. Anchoring release ticks on the latest
  raw timestamp then misassigns slots; the fitted grid fixed it.

## Absolute latency

The figures above are segments. Decode → present leaves out everything
before decode output: sensor readout, ISP and encode, the drone's TX
queue, airtime, FEC, USB into the page, the WASM core and the hand-off to
the main thread, and WebCodecs decode. The page's own `capture→glass (GS)`
segment has that front half but ends at the rAF timestamp, which on these
platforms is 23–28 ms (up to 83 ms on the deep desktop pipeline) before the
real presentation.

Mac, locked, target 4, calm link, one 10 s trace each (p10 / p50 / p90, ms):

| segment | hold | no hold |
|---|---|---|
| capture → page receives the AU | 15.8 / 16.1 / 16.7 | 15.1 / 15.8 / 16.7 |
| page receives → decoded | 1.4 / 2.0 / 2.2 | 1.9 / 2.1 / 2.4 |
| decoded → drawn (held) | 9.5 / 10.4 / 11.1 | 0.1 / 0.2 / 0.2 |
| drawn → BeginFrame | 0 | 7.4 / 10.2 / 11.6 |
| BeginFrame → presentation | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.9 |
| **capture → presentation** | **51.3 / 51.8 / 52.5** | **49.6 / 52.7 / 54.1** |

maburplay's e2e on the Radxa is ~35 ms, but it is a sum of segments that
starts at the encoder (`enc`), while this starts at the frame pts. The two
are not the same reference, so compare them loosely. The biggest single
web cost is Chrome's compositor: 24 ms from BeginFrame to presentation on
the Mac, 27.7 ms on the phone, 17–83 ms on the desktop. Still outside these
numbers:

- **Before the frame pts:** exposure and sensor readout, the same blind spot
  as maburplay's e2e.
- **After the presentation timestamp:** panel scanout (up to one period top
  to bottom) and pixel response.
- **The RTT offset assumes a symmetric path.** The half-duplex link is not
  symmetric, so the capture end carries a few ms of uncertainty, the same
  for every row.

## What this means

- **The loop ports.** A page sees a refresh grid steady enough to run
  `PhaseLock` unchanged, the lock holds, and on the real screen it is flat
  on all three platforms.
- **Two things the shipped design would need:**
  - A wider steering range. One phone's browser grid sat 0.27 Hz off
    nominal, beyond ±200 mfps. The desktop was 0.003 Hz off.
  - The rolling lateness window, not per-second estimates.
- **A page-side hold** (drawing each frame in its release tick's rAF) is
  what keeps the cadence when delivery jitter approaches the period. It
  costs nothing on a calm link, but needs maburplay's ~4 ms margin of slack
  and a fitted rAF grid.
- **Without a hold, a lower margin is possible.** Target 1 saved 3 ms on
  the desktop. With a hold it drops frames.
- **On this desktop the browser's own pipeline depth, not the beat,
  dominates the time to the screen.** A web phase lock is worth more on a
  platform whose pipeline is fixed, like the phone or the Mac, than it
  would be here.
- **End to end, the web GS on the Mac is ~52 ms capture → presentation.**
  About 24 ms of that is Chrome's compositor, which a phase lock cannot
  remove.
