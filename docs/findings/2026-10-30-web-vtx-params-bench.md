# Web GS VTX tab — bench gates

**Status:** gates for the VTX tab and `VENC_MODE` (`docs/web-gs.md` "VTX
tab", `docs/mavlink.md` "Parameters" and "Ground client"). Run on the bench
pair with `maburd` built from the feature branch at its head. The GS kept
its earlier `maburgs`/`maburplay` (the tunnel relay is unchanged); its
MAVLink endpoint was bound to its hotspot address for `mavparam.py`. The
web GS ran in host Chrome (GS mode, the EU 8812 card, the bench link key)
with `maburgs` stopped, so only one ground end commanded the drone. The
drone started at `100@1280x720` with the recorder on at 60 fps and an
explicit `record.size` 1280x720.

## Gate 1 — `mavparam.py list` shows 12 rows → pass

```
RADIO_WALL_MARGN  Real32  1.0
RADIO_LDPC        Bool    1
VENC_MODE         Str     100@1280x720
VENC_SLICES       Int32   4
ENC_BITRATE_MAX   Int32   12000
ENC_AIRTIME       Real32  0.6499999761581421
REC_ENABLE        Bool    1
REC_BITRATE       Int32   40000
REC_FPS           Int32   60
REC_SIZE          Str     1280x720
LOWPWR_ENABLE     Bool    0
MSP_ENABLE        Bool    1
```

## Gate 2 — `set VENC_MODE 120@1728x816` with the recorder on → pass

`IN_PROGRESS 100@1280x720`, then `ACCEPTED 120@1728x816`. `REC_SIZE` read
back `1728x816`. The file had `venc.fps = 120`, `venc.size = "1728x816"`,
`record.size = "1728x816"`, `record.fps` still 60. The drone log:

```
[mav] set VENC_MODE = "120@1728x816" (venc.fps +record.size) in progress
[record] ch1 1728x816 60 fps CBR 40000 kbps on VPE port0, bound before the link (idle)
maburd config: apply VENC_MODE rebuild ok +record.size 490 ms
[mav] set VENC_MODE = "120@1728x816" (venc.fps) applied rebuild 490 ms
```

The sensor clock moved to 119.66 MHz and the link stayed in session. Two
encoder verbs (`set_bitrate`, `set_roi_qp`) failed once inside the
rebuild window and were retried on the next tick (`venc_verb_fail` 0 → 2).

## Gate 3 — the page → pass, with the list 0.2 s over its target

The VTX tab, driven over CDP:

- **List after link-up.** All 12 rows, values identical to gate 1's (the
  new mode and the followed recorder size included), no "no heartbeat"
  tag. Measured with a 50 ms in-page sampler on three reconnects: the first
  4 values arrive with the link, the other 8 about 2 s later, the list
  complete **2.15–2.25 s after link-up** (target: 2 s). See "Link-up list
  loss" below.
- **Refresh mid-session.** The list completed in 181–192 ms (four runs).
- **Bitrate edit.** `ENC_BITRATE_MAX` 12000 → 11000: `sending…` at 11 ms,
  `applying…` at 52 ms, `accepted` at **102 ms** after the commit (target:
  1 s). The drone applied it as a knob in 55 ms. Restored to 12000 the same
  way (102 ms).
- **Mode row.** With the pair equal to the live value the Apply button was
  disabled. Typing fps 100 re-picked the first size of that sensor mode
  (`1728x972`, options `1728x972`/`1280x720`); picking `1280x720` and
  Apply gave `accepted` at 555 ms (rebuild 503 ms), and the page's
  `REC_SIZE` row showed `1280x720` after the post-rebuild re-list. The
  drone's file was back to its starting `venc` and `record` lines.

## Link-up list loss

At link-up the drone answers `PARAM_EXT_REQUEST_LIST` with three `T_MAV`
frames of four values each. Only the first reaches the web GS; values 5–12
come from the client's missing-index re-reads 2 s later. The drone's
counters show every frame sent (`mav tx` frames/packets moved 12/21 per
reconnect: the list, the 8 re-reads, a heartbeat), and a mid-session list
loses nothing. In each recorded reconnect the page's channel readout
changed within about 1.5 s of link-up (112 → 144 → 112 once, 112 → 144 the
next time), the GS's post-link channel move, which is when frames 2 and 3
were in flight. Not investigated further; the list always completed on the
first re-read round.

## Oddities

1. **The "applied" log line drops `+record.size`.** The in-progress line
   and the applier's line name the followed key; `[mav] set … applied` does
   not.
2. **The tab jumps to Stats after the operator opened VTX.** The page's
   "on live → Stats" rule fired after the VTX tab was already in use in
   one connect. The one lost edit of the session happened around it. It
   was typed into the panel, showed no result line and never reached the
   drone; it did not reproduce in later tab round-trips or reconnects.
   Cause not found.

## Device state afterwards

Drone: `/usr/bin/maburd` = this build, the previous binary kept as
`/usr/bin/maburd.pre-vtx`; config back to its starting values. GS:
`/etc/maburgs.toml` restored from `.pre-vtx` (`[mavlink] bind` back to
loopback), `maburgs` running and linked.
