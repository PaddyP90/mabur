# Does the MAVLink parameter tunnel work on the bench pair?

**Status:** hardware gate of the built feature; `docs/mavlink.md` owns the
topic now (wire, drone component, GS endpoint, bench tool).

## Question

Branch `mavlink-config` adds `T_MAV` (`RC_VERSION` 17), the drone's
parameter component (13 rows of `/etc/mabur.toml` as `PARAM_EXT_*`), the
GS's UDP endpoint and `tools/bench/mavparam.py`. On the real pair: does it
link after the flag day, list/set/refuse as documented, keep the file
byte-exact, leave video untouched, and survive a drone restart?

## Rig

- **Build:** `mavlink-config` at 34e72bb7. `out/arm/maburd` (md5
  `f17a4a4f…`), `out/arm64/maburgs` (md5 `6e5a9855…`, Radxa glibc build).
- **Drone:** the bench VTX on its tuned `/etc/mabur.toml` (power_mode
  offset, venc 1280x720 @ 100 fps, 4 slices, `[record]` on at 1920x1080 @
  60 fps, msp on, low power off). No drone config change for this feature;
  its boot defaulted one key (`nack=(section absent)`), as before.
- **GS:** Radxa ZERO 3, two RTL8812EU cards, channel set [64,112,144]
  parked on 144. `[mavlink]` appended to `/etc/maburgs.toml` (overlay, not
  vfat): `enable = true`, `bind = "<gs-ip>"` (its LAN address, for the
  host-run tool), `port = 14550`.
- **Ground app:** `tools/bench/mavparam.py` on the host under
  `nix-shell -p python3Packages.pymavlink`, `GS=<gs-ip> PORT=14550`.
- Link key fingerprint 2263 on both ends.

## Deploy

Drone first, then GS (`docs/deploy.md` clean sequence; stop, verify, swap,
`setsid` start, each its own ssh call). Old binaries kept as
`maburd.pre-mav` / `maburgs.pre-mav`, the GS config as
`maburgs.toml.pre-mav`.

- Between the swaps the new `maburd` logged, every 5 s,
  `maburd: heard an RC frame at RC_VERSION 16 but this build speaks 17 --
  ignoring it ... The pair is half-deployed`. Expected.
- `maburd` boot: `config: 1 key(s) defaulted:`, `link: key 2263`,
  `maburd mav: component sys 1 comp 191, config /etc/mabur.toml`.
- `maburgs` boot: `config: 2 key(s) defaulted:` (`radio.cards`,
  `hop.relay_burst_period_ms`, both pre-existing), `maburgs: link: key
  2263`, `maburgs: mavlink endpoint udp <gs-ip>:14550`. No
  `drone lacks CAP_MAVLINK`. The ladder promoted 0 → 5 (mcs4/40) within
  ~10 s, `frames[clean/trunc/drop]` clean.

## Results

### `list` → 13 rows, all match the file

```
RADIO_WALL_MARGN  Real32  1.0
RADIO_LDPC        Bool    1
VENC_FPS          Int32   100
VENC_SIZE         Str     1280x720
VENC_SLICES       Int32   4
ENC_BITRATE_MAX   Int32   12000
ENC_AIRTIME       Real32  0.6499999761581421
REC_ENABLE        Bool    1
REC_BITRATE       Int32   40000
REC_FPS           Int32   60
REC_SIZE          Str     1920x1080
LOWPWR_ENABLE     Bool    0
MSP_ENABLE        Bool    1
```

`ENC_AIRTIME` is the file's `0.65` as REAL32, printed at double precision
by the tool. Drone log: `[mav] list: 13 values`. One run takes ~1.5 s
including nix-shell start-up.

### `set REC_ENABLE 0` / `1` → ACCEPTED, file byte-exact

- `ACCEPTED  0`. The only change in the file was `enable       = true` →
  `enable       = false` under `[record]` (spacing kept; the two comment
  lines above it untouched; mode `-rw-------` kept). `.pre-param` appeared
  and equalled the pre-set file (same md5). `maburd` kept its pid. Log:
  `[mav] set REC_ENABLE = false (record.enable)`.
- `ACCEPTED  1`: the file md5 equalled the start again
  (`2894a586…`); `.pre-param` now held the `false` version.

### `set VENC_SIZE 1x` → FAILED, nothing written

```
FAILED  1280x720
venc.size: malformed, expected WIDTHxHEIGHT (e.g. 
```

The status text is the loader's message cut at 50 characters (the doc's
rule). Drone log has the full reason:
`[mav] set VENC_SIZE refused: venc.size: malformed, expected WIDTHxHEIGHT
(e.g. "1920x1080")`. File md5 unchanged; no candidate file left in `/etc`.

### Ordering: one refusal, two acceptances, then the reverse

With `record.fps` 60 under `venc.fps` 100 (both inside 16:9 modes for
1280x720):

| set | result | status text |
|---|---|---|
| `VENC_FPS 50` | `FAILED  100` | `record.fps: must be in [1, venc.fps]` |
| `REC_FPS 50` | `ACCEPTED  50` | |
| `VENC_FPS 50` | `ACCEPTED  50` | |
| `VENC_FPS 100` | `ACCEPTED  100` | |
| `REC_FPS 60` | `ACCEPTED  60` | |

After the second `VENC_FPS 50` the diff against the start was exactly the
two `fps` lines; the commented-out `#size = "1920x1080"` line above the live
`size` line was left alone. After the reverse pair the md5 equalled the
start.

### Video unaffected while listing

`ausniff.py --seconds 60 --json` on the GS while `list` ran three times
(all three returned the same 13 values):

```json
{"aus": 6013, "complete": {"1": 3007, "0": 3006}, "incomplete": {}, "salvaged": {}, "nslices": {"4": 6013}, "frame_id_gaps": 0, "resyncs": 0, "bytes": 89030747, "dropped_oversize": 0, "fps": 100.2}
```

A 15 s pass after the final GS restart: 1515 AUs, 101.0 fps, 0 gaps.

### Sideport

`link.mav` / `link.rcf_slot` across the session (cumulative since
`maburgs` start):

| point | mav tx | mav rx | mav reject | rcf_slot au / probe / passthru / timeout |
|---|---|---|---|---|
| after 9 tool runs | 18 | 207 | 17 | 1837 / 124 / 4 / 3 |
| after the sniff's 3 lists | 29 | 650 | 28 | 6014 / 147 / 4 / 3 |
| after the `maburd` restart + 1 list | 31 | 730 | 30 | 6614 / 291 / 1069 / 7 |

The T_MAV sends added no `timeout` or `passthru` releases while linked
(both counters stood still from link-up to the drone restart; the restart's
outage is what moved them afterwards). Drone side, same span:
`mav rx=29/29 tx=358/401 drop=0 reject=0` — every GS frame verified.

### Drone restart

`maburd` restarted (stop, verify, `setsid` start): booted on the file the
tool had written, `link: key 2263`, relinked; the GS rotated its debug
session on the restart. `list` returned the same 13 values. The GS's
counter restart on the new `vtx_nonce` worked: the first post-restart
frames were accepted (`mav rx=2/2 ... reject=0`).

## Oddities

1. **Every drone packet reaches the UDP peer twice on a two-card GS.**
   GS `mav rx` grew ~2/s while the drone sent 1 heartbeat/s. A heartbeat
   listener on the host saw `hb seqs: [245, 245, 246, 246, 247, 247, ...]`.
   The aggregator hands each card's copy of an RC frame to the rc sink
   (no cross-card dedup), and the tunnel forwards both. Harmless to
   `mavparam.py`; a GCS sees duplicate MAVLink sequence numbers (and
   duplicate `PARAM_EXT_ACK`s). `docs/mavlink.md` does not mention it.
2. **`link.mav.reject` counts one per frame the GS sends.** `reject`
   tracked `tx` 1:1 throughout (17/18, 28/29, 30/31) and did not move while
   no client was sending; one bare heartbeat from the host added
   `tx` +1 and `reject` +1 with no drone reply involved. Likeliest cause:
   the other GS card hears the GS's own T_MAV transmission, and
   `on_rc_frame` parses that tagged frame as an untagged drone frame,
   fails, and counts `drop_bad_packet`. Nothing is lost (the drone
   verified every frame), but the counter no longer means "refused
   packets".
3. `ENC_AIRTIME` lists as `0.6499999761581421`: REAL32 printed as a
   double (cosmetic; not exercised with a set).

## Device state at the end

- Drone: new `maburd` running; `/usr/bin/maburd.pre-mav` (the previous
  build) and `.pre-weblock` kept. `/etc/mabur.toml` md5 identical to the
  start; `/etc/mabur.toml.pre-param` present (the file before the last
  set, `record.fps = 50`).
- GS: new `maburgs` running; `maburgs.pre-mav` and
  `/etc/maburgs.toml.pre-mav` kept. `/etc/maburgs.toml` keeps the
  `[mavlink]` section with `enable = true` and `bind = "127.0.0.1"` (the
  shipped default); boot line `mavlink endpoint udp 127.0.0.1:14550`.
  Rollback = remove the `[mavlink]` section and restore `maburgs.pre-mav`,
  paired with `maburd.pre-mav` on the drone.
