# MAVLink hot reload — bench gates

**Status:** gates for the live-apply feature (`docs/mavlink.md` "Applying a
set", `docs/link-adaptation.md` "Rebuilding the encoder pipeline
in-process"). Run on the bench pair with `maburd` and `maburplay` built
from the feature branch at its head; the player in raw DVR mode and the
GS's MAVLink endpoint bound to its LAN address so the host's `mavparam.py`
could reach it. The drone was at 1280x720@100 with the bundle's record
section (`record.size` 1920x1080, which the 100 fps sensor mode cannot
capture, so the recorder channel is absent at boot on this config).

## Tooling

`tools/bench/mavparam.py set` from the host; `tools/bench/ausniff.py` and
`tools/bench/aucadence.py` on the GS against `/dev/shm/mabur-au` (the
repo copies, not the stale one on the GS's root); the drone's
`/tmp/mabur.log` for the `[mav] set …` / `maburd config: apply …` lines and
its `stats:` tail (`mav_apply`, `mav_rebuild`, `venc_verb_fail`,
`snr_mhz`); `ffprobe` on the host for the DVR files. Geometry of the live
stream could not be read from an AU dump: the GDR link carries parameter
sets only at IDRs, which are seconds apart, so the DVR files (which open
on an IDR) were the geometry proof.

## Results

| gate | result | evidence |
|---|---|---|
| 1 bitrate ceiling knob | pass | `ENC_BITRATE_MAX` 12000 → 3000 → 12000: ausniff bytes/s 6.5 → 3.4 → 6.6 Mb/s, AU rate 102–103 fps throughout; applied as `knob` in 53 and 35 ms (the file publish dominates); aucadence offset −0.215 ms before, −0.295 ms after (gate ≤ 0.5 ms) |
| 2 sensor-mode and size rebuilds | pass | `VENC_FPS` 100 → 60: `venc_core: restarted (1280x720@60)`, rebuild 511 ms, trim re-locked at 59.992 Hz, AU rate 62.8; `VENC_SIZE` → 1920x1080: 494 ms, trim 59.988 Hz; back `VENC_SIZE` 1280x720: 565 ms; `VENC_FPS` 100: 485 ms, trim 99.970 Hz, AU rate 102.6. Each set answered IN_PROGRESS then ACCEPTED, 3.2–4.5 s wall from the host including the tool's start-up. maburplay stayed `vsync=locked` across all four; the regulator booked discontinuities and a handful of presentation drops at each rebuild and nothing after. The record channel appeared at the 60 fps modes (`[record] ch1 1920x1080 60 fps CBR 40000 kbps … bound before the link`) and vanished again at 100 fps, as the loader's capture rule says |
| 3 twenty rebuilds | pass | 20 alternating `VENC_SLICES` 2/4 sets, 20/20 ACCEPTED, rebuild 450–549 ms (min/median/max); fd count 21 before, after the first and after the twentieth; RSS 9168 → 9176 → 9200 kB; no `venc fault`, dmesg free of `readiness timeout`, `not sync`, `multidevmap`; AU rate 103 after |
| 4 record keys | pass | `REC_SIZE` 1280x720 created `ch1 1280x720 60 fps CBR 40000 kbps on VPE port0, bound before the link`; `REC_BITRATE` 20000 recreated it at `CBR 20000` (501 ms each); after each rebuild the agent re-issued the GS's standing wish (first without a card: `rec: START refused (err 3)`, NoCard). With an SD card fitted: record on from the GS button → `rec: recording -> record-0019.mp4`; `REC_BITRATE` 40000 mid-recording → `rec: STOP (record-0019.mp4) files=1 dropped=0`, `ch1 … CBR 40000 … bound before the link`, rebuild 637 ms, `rec: START armed` → `recording -> record-0020.mp4` within a second of the rebuild with no button press; stop → `files=2 dropped=0`. Both files decode on the host (536 and 808 frames, 1280x720). Observation, not a gate: both files carry about 4.1 Mb/s on the static bench scene regardless of the 20000/40000 CBR setting, which is the recorder's existing behaviour |
| 5 LDPC | pass (drone side) | `RADIO_LDPC` 0 then 1: `reprogram` 33 and 30 ms, link held (`sent` climbing, state LINKED), AU rate 103, no RCF-slot timeout lines on the GS. The OSD rate row was not observed |
| 6 MSP thread | pass | `MSP_ENABLE` 0: the `mbr-msp` thread gone; 1: `maburd msp: enabled … reading /dev/ttyS2` and the thread back; 35 and 37 ms |
| 7 TX-power margin | pass (margin half) | `RADIO_WALL_MARGN` 2.0 then 1.0: `reprogram` 59 and 62 ms, `SetTxPowerRateDiffs` accepted. The refusal during a calibration sweep was not run on the device (host test `submit_during_calibration_is_refused` covers it) |
| 8 raw DVR roll | pass (maburplay) | one recording across gate 2's return leg: `rec: START (raw)` → `rec: ROLL (record-0205.mp4)` at the 1080→720 IDR → `rec: STOP (1532 samples)`; `record-0204.mp4` 1920x1080, 1553 frames decoded; `record-0205.mp4` 1280x720, 1479 frames decoded. The web GS half was not run (no browser GS on this bench) |
| 9 set at RENDEZVOUS | not run | the tag/session rule is unchanged code; no device run |
| ladder rung 2 | pass | `MABUR_VENC_FAIL_RESTARTS=1` in the wrapper's environment for one start: `VENC_SLICES` 2 → `injected pipeline start failure` → `new pipeline refused` → `previous pipeline restored`; ack FAILED `pipeline refused venc.slices: restored previous`, 536 ms; video back at 103 fps, `mav_apply=0/1 mav_rebuild=2` |
| ladder rung 3 | pass | `=2`: both pipeline-only starts refused → `previous pipeline restored by a cold restart` (`ISP channel ready after 16 ms`, `ISP file loaded`), ack FAILED `… restored previous (cold)`, 8785 ms, the tool's 15 s wait covered it; video back at 103 fps; `venc_verb_fail` 447 over the gap (the agent's re-asserts, as designed). A clean restart afterwards shows no injection line and a normal apply counter |

Numbers that matter for the operator: a knob or reprogram lands in 30–60
ms; a rebuild in 450–570 ms plus the first frame; the worst case, a
rebuild whose restore also fails, costs about 9 s with the link up
throughout, not a 14–17 s respawn.

## Not covered

- The web GS's deferred roll (gate 8's second half) and the calibration
  refusal (gate 7's second half) on a device.
- Long-run soak beyond the twenty rebuilds.
