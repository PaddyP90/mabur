# MAVLink tunnel and drone parameters over the link

The radio link carries MAVLink 2 packets in both directions inside one RC
frame type, `T_MAV`. The first tenant is a parameter component on the drone
that serves thirteen keys of `/etc/mabur.toml` as MAVLink extended
parameters (`PARAM_EXT_*`): a ground end holding the link key can list,
read and set them while linked. A set rewrites the config **file**; the
running `maburd` keeps the values it booted with until it restarts.

On the GS, `maburgs` bridges the tunnel to a MAVLink UDP endpoint, so a
ground app or `tools/bench/mavparam.py` talks to the drone as if it were a
MAVLink system on the network. There is no operator screen for it yet.

## Where the code lives

| Piece | File |
|---|---|
| `T_MAV` frame, `CAP_MAVLINK` | `common/include/mabur/rc_proto.h` |
| the parameter table (ids, keys, types, help) | `common/include/mabur/config_params.h` |
| the vendored MAVLink common dialect | `third_party/mavlink/` (upstream commit in its `VERSION`), included through `common/include/mabur/mav.h` |
| drone: the component (list/read/set, heartbeat) | `drone/src/mav_component.h` |
| drone: per-row accessors and value coding | `drone/src/mav_params.h` |
| drone: line patcher and verified publish | `drone/src/config_write.h` |
| drone: MAV thread, tag and counter check | `drone/src/main.cpp` (`mbr-mav`), `RcAgent::check_mav` |
| GS: the tunnel (shared GS core) | `gs/src/mav_tunnel.h` |
| GS: the UDP endpoint | `gs/src/mav_endpoint.h` |

## The wire: `T_MAV`

`T_MAV = 8`, `RC_VERSION` 17. Same layout both directions; only the GS to
drone direction carries the tag:

```
u16 magic "RC" | u8 version | u8 type = T_MAV |
u32 counter     GS -> drone: per-session, starts at 1, strictly increasing
                drone -> GS: per-process, observability only
u16 len         payload byte count, at most kMavMaxPayload (600)
u8[len]         one or more whole MAVLink 2 packets, back to back
[u8[8] tag]     GS -> drone only: SipHash over the bytes before it || TagCtx
u16 crc16
```

- A packet never spans two frames. Both ends pack as many whole packets as
  fit in 600 bytes (three 161-byte `PARAM_EXT_VALUE`s, or a 21-byte
  heartbeat plus three).
- The RC layer treats the payload as opaque. Each end splits it with a
  MAVLink parser; a bad MAVLink CRC (which is also what refuses a message
  id the common dialect does not know), a MAVLink 1 packet, or a packet
  cut off at the end of a payload is dropped and counted. MAVLink 2 only.
- **GS to drone** is tagged with `TagCtx{vrx_nonce, vtx_nonce, seq32 =
  counter}`, the NACK rule (`docs/link-pairing.md`). The drone accepts a
  frame only under its published session and only with a counter greater
  than the last one it accepted in that session. A frame that fails the
  tag or the counter, or arrives with no published session, is dropped
  whole and raises `auth_reject` (Telem flags bit1); a frame that fails to
  parse is dropped and counted without it. The GS restarts its counter at
  1 when the session's `vtx_nonce` changes.
- **Drone to GS** is untagged, like `T_TELEM`, and goes out on the Telem
  path (control radiotap, the shared Telem 802.11 sequence) at most one
  frame every `kMavUplinkMinMs` (50 ms), which caps the tunnel near
  12 kB/s on the drone's uplink.
- **GS to drone** frames are offered to the RCF slotter like an RCF, so
  they land in the drone's inter-AU idle, and are never sent while a
  calibration sweep holds the GS radio-silent. A frame goes out only when
  a ground app has sent something.
- `CAP_MAVLINK` (`0x0008`) in DISC_ACK `chip_caps` says the drone answers
  `T_MAV`. It is display-only: with `[mavlink] enable`, `maburgs` logs
  `maburgs: mavlink: drone lacks CAP_MAVLINK` once per session when the bit
  is missing, and it gates nothing on it.

## Ids

- The drone's component answers as system 1, component 191
  (`MAV_COMP_ID_ONBOARD_COMPUTER`). A flight controller on the same drone
  would be component 1, the MAVLink convention.
- Ground senders use their own ids (QGroundControl sends 255/190).
  `maburgs` originates no packets of its own: it forwards bytes unchanged
  in both directions.

## The drone component

The MAV thread (`mbr-mav`) owns the component: it takes `T_MAV` frames off
their own queue, checks the tag and counter, feeds the payload in, and
sends queued replies. The config write fsyncs flash, so it stays off the
agent and TX threads.

- **Heartbeat.** Once a second while a session is published:
  `MAV_TYPE_ONBOARD_CONTROLLER`, `MAV_AUTOPILOT_INVALID`,
  `MAV_STATE_ACTIVE`. Suppressed while a calibration phase sweeps, the
  same rule as the periodic Telem. This is what a ground app discovers
  the component by.
- **Routing.** A message is for the component when its target system is 0
  or 1 and its target component is 0 or 191. Served:
  `HEARTBEAT` (ignored), `PARAM_EXT_REQUEST_LIST`,
  `PARAM_EXT_REQUEST_READ`, `PARAM_EXT_SET`. Anything else, or anything
  for another target, is dropped and counted.
- **List.** One `PARAM_EXT_VALUE` per table row, in table order,
  `param_count` 13 and `param_index` = the row. Values are read by loading
  the file with the boot loader (`mabur::load_config`) on every request,
  so a list shows a set made a moment ago or an ssh edit. A file that no
  longer loads answers nothing and logs the loader's reason.
- **Read.** By `param_index` (when not -1) or by `param_id`: one
  `PARAM_EXT_VALUE`. An unknown id or index gets no reply (the protocol
  has no negative read answer; the ground end times out) and is logged.
- **Set.** One key per set, answered with one `PARAM_EXT_ACK`:
  - unknown id: `PARAM_ACK_VALUE_UNSUPPORTED`, value zeroed;
  - a `param_type` other than the row's, a Bool byte other than 0/1, a
    non-finite REAL32, or a CUSTOM value with no NUL in its 128 bytes or a
    non-printable byte: `PARAM_ACK_VALUE_UNSUPPORTED` with the current
    value; there is no cross-type coercion;
  - otherwise the file's line for the key is patched in place (spacing and
    trailing comment kept byte for byte), the candidate is written next to
    the file and verified with `mabur::load_config`, and only a candidate
    the loader accepts replaces the file. Success:
    `PARAM_ACK_ACCEPTED` with the value read back from the published file.
    Failure: `PARAM_ACK_FAILED` with the current, unchanged value, followed
    by one `STATUSTEXT` (`MAV_SEVERITY_ERROR`) carrying the reason
    (`<field>: <why>`, the loader's message without its file and line,
    cut to 50 characters). A key whose line is missing or commented out
    is `FAILED` with `<section.key>: key not found`; nothing is written.
  - A set to the value the file already holds writes nothing and answers
    `ACCEPTED`, so a ground end may re-send a set whose ack it lost.
  - Each set is one `[mav] set ...` line in `maburd`'s log
    (`docs/observability.md`).

`ACCEPTED` always means published to disk. The read-patch-publish
sequence holds the process-wide config-write lock, so a calibration apply
on another thread and a parameter set never lose each other's change.
There is no arm-state gate: a set in flight is the operator's call.

### `.pre-param`

Every set that changes the file saves the previous file as
`/etc/mabur.toml.pre-param` before replacing it (written then renamed, so
a crash part way through leaves the previous backup intact). It is the
undo over ssh; over the air the undo is setting the old value again. It
sits beside `.pre-cal` in the `.pre-*` convention (`docs/deploy.md`).

## Parameters

The table in `common/include/mabur/config_params.h` is the single source:
its order is the `param_index` order on the wire, and the help strings
are what an operator UI shows. Types on the wire: Bool is
`MAV_PARAM_EXT_TYPE_UINT8` (byte 0 is 0 or 1), Int32 and Real32 are
`INT32`/`REAL32` little-endian in bytes 0..3, Str is `CUSTOM`
(NUL-terminated ASCII, at most 127 characters, written into the TOML as a
quoted string).

| # | param_id | TOML key | type | help |
|---|---|---|---|---|
| 0 | `RADIO_WALL_MARGN` | `radio.wall_margin_db` | Real32 | Safety margin below the calibrated TX-power walls; raise to lower TX power |
| 1 | `RADIO_LDPC` | `radio.ldpc` | Bool | LDPC coding on every frame; turn off only for a ground card that cannot decode it |
| 2 | `VENC_FPS` | `venc.fps` | Int32 | Camera frame rate, which also selects the sensor's field-of-view mode |
| 3 | `VENC_SIZE` | `venc.size` | Str | Encoder output resolution, must match the fps mode's aspect ratio |
| 4 | `VENC_SLICES` | `venc.slices` | Int32 | H.265 row slices on the link channel; 1 disables slice salvage |
| 5 | `ENC_BITRATE_MAX` | `encoder.bitrate_max_kbps` | Int32 | Bitrate ceiling, kbps |
| 6 | `ENC_AIRTIME` | `encoder.airtime_budget` | Real32 | Share of the link's real capacity the video may use |
| 7 | `REC_ENABLE` | `record.enable` | Bool | Enable the onboard microSD recorder |
| 8 | `REC_BITRATE` | `record.bitrate_kbps` | Int32 | Constant bitrate of the onboard recording |
| 9 | `REC_FPS` | `record.fps` | Int32 | Onboard recording frame rate, at most the camera's fps |
| 10 | `REC_SIZE` | `record.size` | Str | Onboard recording resolution, same aspect ratio as the camera's |
| 11 | `LOWPWR_ENABLE` | `low_power.enable` | Bool | Lower bitrate and frame rate while disarmed to keep the VTX cool; needs msp.enable for arm state |
| 12 | `MSP_ENABLE` | `msp.enable` | Bool | Flight-controller link for the OSD and arm state |

Ranges are the loader's, not repeated here: whatever `maburd` refuses to
boot on, a set refuses.

### Ordering rules

There is no transaction: each set is verified against the whole file as it
stands, so keys that depend on each other must be set in a working order.
A set out of order is refused with the loader's reason and changes
nothing; send the other key first. The rules the loader enforces:

- `REC_FPS` ≤ `VENC_FPS` (while the recorder is on): lower `REC_FPS`
  before lowering `VENC_FPS`; raise `VENC_FPS` before raising `REC_FPS`.
- `VENC_SIZE` must fit the camera mode `VENC_FPS` selects
  (`docs/camera-modes.md`: up to 90 fps 1920x1080, 91-100 1728x972,
  101-120 1728x816; same aspect ratio, no larger). Going above 90 fps,
  set the smaller `VENC_SIZE` first; coming back down, set `VENC_FPS`
  first.
- `REC_SIZE` above 1920x1080 needs `VENC_FPS` ≤ 30 first.
- `LOWPWR_ENABLE = 1` needs `MSP_ENABLE = 1` first; `MSP_ENABLE = 0` needs
  `LOWPWR_ENABLE = 0` first.

Two loader rules involve keys the table does not expose:

- With `LOWPWR_ENABLE = 1`, `VENC_FPS` cannot drop below `low_power.fps`
  and `ENC_BITRATE_MAX` cannot drop below `low_power.bitrate_kbps`.
- `REC_SIZE` must keep `VENC_SIZE`'s aspect ratio while the recorder is on.

The 101-120 fps mode (1728x816) has a different aspect ratio from the
16:9 modes below it, so moving between 100 fps or less and 101 fps or more
needs `VENC_FPS` and `VENC_SIZE` to change together. No single set can do
that, so that move is an ssh edit of `/etc/mabur.toml`.

## The GS endpoint

`maburgs` opens one UDP socket when `[mavlink] enable`:

```toml
[mavlink]
enable = true
bind   = "127.0.0.1"   # the GS's LAN address to let a ground app on the LAN connect
port   = 14550         # MAVLink's conventional GCS port
```

`bind` is a dotted IPv4 address; `port` is 1..65535. A port that cannot be
bound is a boot warning, not fatal: the video link does not depend on it.
Boot log: `maburgs: mavlink endpoint udp <bind>:<port>` or
`maburgs: mavlink endpoint disabled`.

- **Last peer.** The peer is whoever sent the most recent datagram;
  QGroundControl, MAVProxy and pymavlink all open with a heartbeat. Each
  datagram's bytes go to the tunnel, which splits them into packets and
  queues whole ones.
- **Outbound** packets are packed into `T_MAV` frames once per core-loop
  tick. While `maburgs` is not in a session they are dropped and counted:
  a ground app's heartbeats before link-up are not replayed later.
- **Inbound**: each MAVLink packet from the drone goes to the peer as its
  own datagram. With no peer, or when `sendto` fails, the datagram is
  dropped: the socket never blocks and never retries. Retrying a request
  whose answer was lost is the ground app's job.
- Polled from the core loop next to the calibration control socket; at
  most 64 datagrams per tick, so a flooding client cannot hold the loop.
- Sideport: `link.mav{tx, rx, reject, peer}` (`docs/observability.md`);
  maburtop shows `mav <tx>/<rx>` on its top bar.

### Trust boundary

Over the air the tunnel is authenticated by mabur's SipHash tag, bound to
the session nonces (`docs/link-pairing.md`): only a GS holding the link
key, in the current session, can make the drone act. The UDP endpoint
sits inside that boundary, like the calibration control socket: **whoever
can reach the socket is trusted** and can rewrite the drone's config. The
default bind is loopback; binding the GS's LAN address is a deliberate
choice that lets anything on that LAN in.

MAVLink 2 message signing stays off in both directions. It needs a wall
clock the drone does not have, and a second key next to the link key; the
SipHash tag is the authentication. Signing between a ground app and a
flight controller would pass through the tunnel untouched: mabur never
rewrites packets.

## Bench tool

`tools/bench/mavparam.py` runs on the host with pymavlink (NixOS:
`nix-shell -p python3Packages.pymavlink`) and talks UDP to the endpoint.
`GS=` (the GS address) and `PORT=` come from the environment, with no
default:

```
GS=<gs-ip> PORT=14550 nix-shell -p python3Packages.pymavlink --run "python3 tools/bench/mavparam.py list"
GS=<gs-ip> PORT=14550 nix-shell -p python3Packages.pymavlink --run "python3 tools/bench/mavparam.py get VENC_FPS"
GS=<gs-ip> PORT=14550 nix-shell -p python3Packages.pymavlink --run "python3 tools/bench/mavparam.py set REC_ENABLE 0"
```

It sends one heartbeat so the endpoint learns the peer, waits up to 5 s
for the drone's own `HEARTBEAT` (system 1, component 191), then runs the
one subcommand:

- `list` requests the whole table and prints one `id  type  value` row
  per parameter, in `param_index` order, retrying missing indices
  individually; a parameter still missing after the retries is a
  non-zero exit.
- `get ID` reads one parameter by id (1 s timeout, 3 retries) and prints
  its `id  type  value` row.
- `set ID VALUE` looks `ID` up in its own copy of the 13-row table (ids
  and types mirrored from `common/include/mabur/config_params.h`) to
  encode `VALUE` for the wire, sends the set (2 s timeout, 3 retries),
  and prints the ack result (`ACCEPTED`/`FAILED`/`VALUE_UNSUPPORTED`/
  `IN_PROGRESS`) and value, then any `STATUSTEXT` that follows within 1 s.

Exit 0 on an ACCEPTED set or a successful list/get, 1 on a runtime
failure (no heartbeat, a request that times out after retries, an
incomplete list, or a non-ACCEPTED ack), 2 on a usage error (missing
`GS`/`PORT`, a bad command line, an unknown parameter id, or a value
that doesn't parse for its type).

The endpoint binds loopback by default, so reaching it from the host means
setting `[mavlink] bind` to the GS's LAN address in the GS's live config
(the GS `/config` is vfat: sync after the edit, `docs/deploy.md`).

## Later tenants

Designed for, not built. Each is more traffic through the same tunnel and
one more case in the drone's routing; none needs a wire change.

- **Flight controller over MAVLink.** A drone `[fc]` section names the
  UART; the MAV thread runs a second parser on it, packets for component 1
  go to the UART, and the FC's packets go up the tunnel under a rate
  filter (a low stream profile requested with
  `MAV_CMD_SET_MESSAGE_INTERVAL`, or a byte budget by message id). Arm
  state comes from the FC heartbeat's `MAV_MODE_FLAG_SAFETY_ARMED` into
  the input low-power mode reads today.
- **Firmware upload / MAVLink FTP.** FTP packets are tunnel payload; the
  tenant is a file-service component on the drone.
- **Calibration over MAVLink.** Needs a mabur dialect (running `mavgen`);
  calibration keeps its own `T_CAL_CMD`/`T_CAL_RESULT` frames until there
  is another reason to.
- **Operator UIs.** A screen in `maburplay` and a form in the web GS
  render from the parameter table: label, help, a widget per type, the
  current value from a list, and the ack plus any status text as the
  result. They sequence dependent sets by the ordering rules above.
