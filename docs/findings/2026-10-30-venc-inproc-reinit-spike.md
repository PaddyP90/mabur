# Same-PID encoder pipeline restart on the Star6E — spike

**Status:** positive result, twice over: a full `venc_core_stop()` +
`venc_core_start()` works (8.5 s), and a pipeline-only restart that keeps
the MI module clients connected works in about 0.6 s to the first frame
of the new mode. Built: `ConfigApplier` drives this in-process rebuild
for a `VENC_FPS`/`VENC_SIZE`/`VENC_SLICES`/`REC_*` MAVLink parameter
set, with the full stop/start and the wrapper-respawn exit as its
fallback ladder — `docs/mavlink.md` ("Applying a set") and
`docs/link-adaptation.md` ("Rebuilding the encoder pipeline in-process")
now record it. An encoder fault (as opposed to a parameter-set rebuild)
still exits: see those pages for what the ladder does and does not
cover. The spike's binary, CMake target and the two throwaway restart
functions are not in the tree.

## Question

Can `maburd` rebuild the MI sensor → ISP → VENC pipeline in its own
process, including into a different sensor mode, and get a live picture
back? The upstream waybeam record (its
`STAR6E_SINGLE_PID_REINIT_FINDINGS.md`, imx335, 2026-06-07) said no:
four in-process reset strategies each came back with a dead stream, and
the conclusion was that the VIF/VPE/ISP channel state is pinned to the
task in the kernel driver and released only by `execv`. That record is
why a fold-in bring-up failure is fatal by design, and why the hot-reload
design for the MAVLink parameters (`docs/mavlink.md`) was about to treat
`VENC_SIZE`, `REC_SIZE`, `REC_ENABLE` off→on and a sensor-mode-crossing
`VENC_FPS` as restart-only keys.

## Method

A standalone ARM binary linking `mabur_drone_core` and `venc_core` (the
same archives `maburd` links, built in the `build-arm-glibc` tree with the
OpenIPC glibc toolchain), run on the bench drone (SSC338Q, the waybeam
IMX415 driver, `/etc/sensors/imx415_greg_fpvXIX_colortrans.bin`) with
`S00mabur` stopped so nothing else held the MI devices. For each config
path on its command line it:

1. `mabur::load_config(path)` → `venc_core_start(&cfg.venc.core, &cb)`,
   timing the call;
2. sleeps 2 s, then counts `VencStats::frames_encoded` over a 6 s window
   and reads `sensor_mhz`;
3. takes a `venc_snapshot_jpeg()` (quality 80) and writes it to `/tmp`;
4. `venc_core_stop()`, timing the call; sleeps 1 s; next config.

No frame-ring consumer was attached, so `full_drops` climbs as the
`/mabur_f` ring fills; the encoder keeps running regardless and
`frames_encoded` still counts. No record channel (`venc_record_configure`
was never called), no radio, no RcAgent. `dmesg` was cleared before each
run and read after.

Config variants were the drone's live `/etc/mabur.toml` with only the
`[venc]` `size`/`fps` lines (and `[record] size` where the aspect ratio
rule required it) changed:

| | `venc.size` | `venc.fps` | sensor mode (`docs/camera-modes.md`) |
|---|---|---|---|
| A | 1280x720 | 100 | 1728x972@100 |
| B | 1920x1080 | 60 | 1920x1080@90 |
| C | 1728x816 | 120 | 1728x816@120 |

## Results

Run 1, four starts of A in one PID (three stop/start cycles):

| cycle | start | frames in 6 s | stop |
|---|---|---|---|
| 0 (cold) | 10 664 ms | 600 (100.0 fps) | 188 ms |
| 1 | 8 570 ms | 600 (100.0 fps) | 189 ms |
| 2 | 8 570 ms | 600 (100.0 fps) | 189 ms |
| 3 | 8 570 ms | 600 (100.0 fps) | — |

Run 2, A → B → C → A → B in one PID (four sensor-mode changes):

| cycle | config | start | frames in 6 s | `sensor_mhz` | snapshot |
|---|---|---|---|---|---|
| 0 (cold) | A 1280x720@100 | 10 634 ms | 600 (100.0 fps) | 99 970 | 40 739 B, real picture |
| 1 | B 1920x1080@60 | 8 522 ms | 361 (60.2 fps) | 59 988 | 81 956 B, real picture |
| 2 | C 1728x816@120 | 8 516 ms | 718 (119.7 fps) | 119 660 | 54 826 B, real picture |
| 3 | A 1280x720@100 | 8 567 ms | 600 (100.0 fps) | 99 970 | 41 282 B, real picture |
| 4 | B 1920x1080@60 | 8 565 ms | 361 (60.2 fps) | 59 992 | 82 415 B, real picture |

Every stop took 229–251 ms. Every snapshot was a correctly exposed
picture of the bench room at the new resolution (the three sizes are
visibly different crops of the same scene; mode 4's 119.660 Hz ceiling
matches `docs/findings/2026-10-19-configurable-fps-bench.md`). No
`venc fault` callback fired. The process exited 0 both runs; no join
hung.

`dmesg` after run 2 held, per start, the same `[MI ERR]` lines a single
cold start in a fresh PID prints (`_MI_SYS_IMPL_GetInputPortInfo not
found` ×4, `MI_VPE_IMPL_GetChannelAttr Invalid channel id: 0`,
`MI_SNR_IMPL_QueryResCount ... fail` ×2, `MI_VIF_IMPL_SetDevAttr
workmode 3 ... not support`), plus one line only a restart prints:
`MI_VIF_IMPL_Init[2537]: MI_VIF already Inited, return ok !`. Each stop
logs `client [pid] disconnected` for venc/sys/sensor/vif/vpe and two
warnings, `MI_SNR_IMPL_DeInit: have another bin connect MI_SENSOR not
Deinit` and the VIF equivalent, which are the SDK noticing the same
process still holds its other modules. None of the upstream failure
signature (`ISP channel readiness timeout`, `CmdLoadBinFile failed`,
`not sync err`) appeared.

## Round 2: where the 8.5 s goes, and the pipeline-only restart

`docs/findings/2026-09-07-boot-time.md` ("Where the 10.6 s goes")
attributes 10.4 s of a warm `venc_core_start()` to five first calls into
different MI modules, each asleep 2.04–2.08 s in the vendor kernel's
device open; it is a restart-only penalty, absent on a cold boot. A
`venc_core_stop()` ends with `MI_SYS_Exit()` and `dlclose` of the MI
libraries, which disconnects every module client, so the next start pays
the penalty again: 8.5 s is four re-opens plus the ~0.3 s the pipeline
itself takes (VIF was the one module that stayed connected, hence the
`MI_VIF already Inited` line).

So round 2 added two throwaway functions and re-ran the same five-config
sequence with the first config `venc_core_start()`ed and each following
one applied by a restart that never leaves the MI_SYS lifetime:

- `star6e_runtime_pipeline_restart(ctx, cfg)` in `star6e_runtime.c`: the
  runtime teardown minus `MI_SYS_Exit`/`star6e_mi_deinit` (CUS3A request
  stop, `star6e_controls_reset`, `star6e_pipeline_stop`, CUS3A join), then
  the runtime init minus `star6e_mi_init`/`MI_SYS_Init`/IPU scrub (copy
  the new `VencCfg`, `g_running = 1`, clear the fault text,
  `star6e_pipeline_cus3a_reset`, `star6e_pipeline_start`,
  `star6e_runtime_apply_startup_controls`).
- `venc_core_restart(cfg)` in `venc_core.c`: clear `g_started`, request
  the encoder loop to stop and join its thread, call the above, set
  `g_started`, spawn the encoder thread again.

Same drone, same configs, same day:

| cycle | config | call | first frame after return | frames in 6 s | `sensor_mhz` | trim |
|---|---|---|---|---|---|---|
| 0 | A 1280x720@100 | start 10 645 ms | 91 ms | 600 (100.0 fps) | 99 970 | locked, 1 round |
| 1 | B 1920x1080@60 | **restart 462 ms** | 105 ms | 360 (60.0 fps) | 59 992 | locked, 1 round |
| 2 | C 1728x816@120 | **restart 500 ms** | 79 ms | 718 (119.7 fps) | 119 660 | at the mode ceiling |
| 3 | A 1280x720@100 | **restart 457 ms** | 92 ms | 600 (100.0 fps) | 99 970 | locked, 2 rounds |
| 4 | B 1920x1080@60 | **restart 488 ms** | 106 ms | 360 (60.0 fps) | 59 992 | locked, 1 round |

Final `venc_core_stop()` 268 ms. Every restart printed `ISP channel ready
after 15–26 ms`. The sensor-rate trim (`[sensor] cal`) re-armed and
re-locked after every restart, so the phase lock's input survives a
rebuild. Snapshots were real pictures at each size. `dmesg` for the whole
run shows each of the five modules connected **once** and disconnected
once (at the final stop), and the per-start `[MI ERR]` set five times
over; no new line kinds.

A sensor-mode change therefore costs about **0.6 s of no video** inside
a running process (restart call ~0.5 s, first frame ~0.1 s later), not
8.5 s and not a 14–17 s respawn.

## Why this differs from the upstream negative

Not established; candidates, in the order they seem likely:

- The fold-in's teardown order (`star6e_runtime_teardown`: stop the CUS3A
  request, `star6e_pipeline_stop`, join the 3A thread, then `MI_SYS_Exit`)
  is a different sequence from the levers upstream tried one at a time.
- A different sensor and driver (IMX415 on the waybeam driver vs imx335).
- Upstream's test harness also carried a deadline watchdog and OSD
  destroy paths that this spike does not.

The upstream mechanism claim (kernel per-task channel state released only
by `execv`) is contradicted by this result on this board; it should not
be cited as a reason to avoid an in-process rebuild on this hardware.

## Not covered

- Whether the pipeline-only restart recovers a *faulted* pipeline (both
  rounds restarted from a clean stop); a wedged ISP may still need the
  full stop/start, or the process boundary.
- A restart inside a running `maburd`: the frame-ring consumer
  (`FrameSource`) re-attaching to a ring that was destroyed and recreated,
  RcAgent's verbs arriving during the ~8.5 s rebuild, the record channel
  (`venc_record_attach` runs inside the pipeline start, so a rebuild
  recreates it in the right bind order, but that was not exercised), the
  radio and TX threads sharing the CPU with the rebuild.
- Long-run stability: five cycles in a few minutes, no thermal soak, no
  count of how many rebuilds one process survives.
- A rebuild after an encoder *fault* (a wedged ISP) rather than after a
  clean stop; the upstream record's "wedged ISP" case may still need the
  process boundary.
- Any sensor mode above 1920x1080 (the recorder's 3840x2160@30).

## What this changes

A size or sensor-mode change costs about 0.6 s of no video inside
`maburd` (pipeline-only restart), with the radio, the control link and
the MAVLink component up throughout, instead of the 14–17 s cold respawn
with the link down. The hot-reload design can treat every one of the
thirteen MAVLink parameters as live, with a "rebuild the pipeline" apply
path for the keys that need one. The full `venc_core_stop()` +
`venc_core_start()` (8.5 s) remains the fallback when the pipeline-only
restart fails.
