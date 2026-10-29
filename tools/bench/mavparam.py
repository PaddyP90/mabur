#!/usr/bin/env python3
"""mavparam.py -- list/get/set the drone's config parameters over maburgs's
MAVLink UDP endpoint (docs/mavlink.md).

Usage:
  GS=<gs-ip> PORT=14550 python3 tools/bench/mavparam.py list
  GS=<gs-ip> PORT=14550 python3 tools/bench/mavparam.py get ID
  GS=<gs-ip> PORT=14550 python3 tools/bench/mavparam.py set ID VALUE

GS (the GS's MAVLink endpoint address) and PORT come from the
environment, with no default (maburgs's `[mavlink] bind`/`port`). Talks
to the drone's parameter component (system 1, component 191) through
maburgs's last-peer UDP endpoint, using MAVLink's extended-parameter
protocol: PARAM_EXT_REQUEST_LIST / PARAM_EXT_REQUEST_READ / PARAM_EXT_SET
answered with PARAM_EXT_VALUE / PARAM_EXT_ACK, plus a STATUSTEXT after a
refused set.

Exit: 0 on an ACCEPTED set or a successful list/get; 1 on a runtime
failure (no heartbeat from the drone, a request that timed out after
retries, an incomplete list, or an ack other than ACCEPTED); 2 on a
usage error (missing GS/PORT, a bad command line, an unknown parameter
id, or a value that doesn't parse for its type).
"""
import os
import struct
import sys
import time

# pymavlink is imported lazily, inside main() after the usage checks below:
# it is only on sys.path inside the project's nix-shell, and a bare
# `python3 tools/bench/mavparam.py` (no GS set) must still print usage and
# exit 2 rather than fail on the import. main() binds this name at module
# scope (`global mavutil`) before any other function needs it.
mavutil = None

DRONE_SYSTEM = 1
DRONE_COMPONENT = 191  # MAV_COMP_ID_ONBOARD_COMPUTER

# The 13-row parameter table, id -> wire type ('b' Bool, 'i' Int32,
# 'f' Real32, 's' Str/CUSTOM). common/include/mabur/config_params.h is the
# source of truth for ids and types; this copy is kept in sync by hand.
TABLE = [
    ("RADIO_WALL_MARGN", "f"),
    ("RADIO_LDPC", "b"),
    ("VENC_FPS", "i"),
    ("VENC_SIZE", "s"),
    ("VENC_SLICES", "i"),
    ("ENC_BITRATE_MAX", "i"),
    ("ENC_AIRTIME", "f"),
    ("REC_ENABLE", "b"),
    ("REC_BITRATE", "i"),
    ("REC_FPS", "i"),
    ("REC_SIZE", "s"),
    ("LOWPWR_ENABLE", "b"),
    ("MSP_ENABLE", "b"),
]
TYPE_BY_ID = dict(TABLE)

# MAV_PARAM_EXT_TYPE values (common.xml), matching config_params.h's
# mav_type(): Bool -> UINT8, Int32 -> INT32, Real32 -> REAL32, Str -> CUSTOM.
MAV_TYPE = {"b": 1, "i": 6, "f": 9, "s": 11}
TYPE_NAME = {"b": "Bool", "i": "Int32", "f": "Real32", "s": "Str"}
NAME_BY_MAV_TYPE = {v: TYPE_NAME[k] for k, v in MAV_TYPE.items()}


def encode_value(wtype, text):
    """CLI value text -> the 128-byte wire value for `wtype`."""
    if wtype == "b":
        low = text.lower()
        if low in ("1", "true"):
            buf = bytes([1])
        elif low in ("0", "false"):
            buf = bytes([0])
        else:
            raise ValueError("not 0/1/true/false: %r" % text)
    elif wtype == "i":
        buf = struct.pack("<i", int(text))
    elif wtype == "f":
        buf = struct.pack("<f", float(text))
    else:  # 's'
        b = text.encode("ascii")
        if len(b) > 127:
            raise ValueError("value too long (max 127 characters)")
        buf = b + b"\x00"
    return buf.ljust(128, b"\x00")


def decode_value(wtype, msg):
    """The wire value out of a PARAM_EXT_VALUE/PARAM_EXT_ACK message,
    formatted for `wtype`. pymavlink decodes the fixed 'char' array field
    by stripping trailing NUL bytes; it keeps the raw (pre-ascii-decode)
    bytes on `_param_value_raw` when the message came off the wire, but
    older pymavlink or a hand-built message may only offer the ascii
    `param_value` string, so both are handled."""
    raw = getattr(msg, "_param_value_raw", None)
    if raw is None:
        raw = msg.param_value
    if isinstance(raw, str):
        raw = raw.encode("latin1", errors="replace")
    else:
        raw = bytes(raw)
    if wtype == "b":
        return str(raw[0] if raw else 0)
    if wtype == "i":
        return str(struct.unpack("<i", raw[:4].ljust(4, b"\x00"))[0])
    if wtype == "f":
        return repr(struct.unpack("<f", raw[:4].ljust(4, b"\x00"))[0])
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")  # 's'


def recv_one(conn, msgtype, timeout):
    """The next `msgtype` message from the drone component (1, 191) within
    `timeout` seconds, or None."""
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            return None
        msg = conn.recv_match(type=msgtype, blocking=True, timeout=remaining)
        if msg is None:
            return None
        if msg.get_srcSystem() == DRONE_SYSTEM and msg.get_srcComponent() == DRONE_COMPONENT:
            return msg


def wait_for_drone(conn, timeout=5.0):
    return recv_one(conn, "HEARTBEAT", timeout) is not None


def collect_values(conn, results, idle_s):
    """Read PARAM_EXT_VALUE messages into results[param_index] until
    `idle_s` passes with no new one landing. Returns the param_count the
    drone reported, or None if nothing arrived."""
    count = None
    last = time.time()
    while time.time() - last < idle_s:
        msg = recv_one(conn, "PARAM_EXT_VALUE", idle_s - (time.time() - last))
        if msg is None:
            break
        last = time.time()
        count = msg.param_count
        wtype_name = NAME_BY_MAV_TYPE.get(msg.param_type, "?")
        results[msg.param_index] = (msg.param_id, wtype_name, decode_value(TYPE_BY_ID.get(msg.param_id, "s"), msg))
    return count


def cmd_list(conn):
    conn.mav.param_ext_request_list_send(DRONE_SYSTEM, DRONE_COMPONENT)
    results = {}
    count = collect_values(conn, results, idle_s=3.0)
    if count is None:
        print("no PARAM_EXT_VALUE received", file=sys.stderr)
        return 1
    for _ in range(3):
        missing = [i for i in range(count) if i not in results]
        if not missing:
            break
        for idx in missing:
            conn.mav.param_ext_request_read_send(DRONE_SYSTEM, DRONE_COMPONENT, b"", idx)
        collect_values(conn, results, idle_s=1.0)
    missing = [i for i in range(count) if i not in results]
    for i in sorted(results):
        pid, wtype_name, value = results[i]
        print("%-16s  %-6s  %s" % (pid, wtype_name, value))
    if missing:
        print("missing %d/%d parameters after retries: %s" % (len(missing), count, missing), file=sys.stderr)
        return 1
    return 0


def cmd_get(conn, pid):
    pid_b = pid.encode("ascii")
    msg = None
    for _ in range(3):
        conn.mav.param_ext_request_read_send(DRONE_SYSTEM, DRONE_COMPONENT, pid_b, -1)
        msg = recv_one(conn, "PARAM_EXT_VALUE", 1.0)
        if msg is not None:
            break
    if msg is None:
        print("no PARAM_EXT_VALUE for %s after 3 retries" % pid, file=sys.stderr)
        return 1
    wtype_name = NAME_BY_MAV_TYPE.get(msg.param_type, "?")
    print("%-16s  %-6s  %s" % (msg.param_id, wtype_name, decode_value(TYPE_BY_ID.get(msg.param_id, "s"), msg)))
    return 0


def cmd_set(conn, pid, value):
    wtype = TYPE_BY_ID.get(pid)
    if wtype is None:
        print("unknown parameter id: %s" % pid, file=sys.stderr)
        return 2
    try:
        raw = encode_value(wtype, value)
    except ValueError as exc:
        print("bad value for %s: %s" % (pid, exc), file=sys.stderr)
        return 2
    pid_b = pid.encode("ascii")
    ack = None
    for _ in range(3):
        conn.mav.param_ext_set_send(DRONE_SYSTEM, DRONE_COMPONENT, pid_b, raw, MAV_TYPE[wtype])
        ack = recv_one(conn, "PARAM_EXT_ACK", 2.0)
        if ack is not None:
            break
    if ack is None:
        print("no PARAM_EXT_ACK for %s after 3 retries" % pid, file=sys.stderr)
        return 1
    result_names = {
        mavutil.mavlink.PARAM_ACK_ACCEPTED: "ACCEPTED",
        mavutil.mavlink.PARAM_ACK_IN_PROGRESS: "IN_PROGRESS",
        mavutil.mavlink.PARAM_ACK_FAILED: "FAILED",
        mavutil.mavlink.PARAM_ACK_VALUE_UNSUPPORTED: "VALUE_UNSUPPORTED",
    }
    result = result_names.get(ack.param_result, str(ack.param_result))
    print("%s  %s" % (result, decode_value(wtype, ack)))
    status = recv_one(conn, "STATUSTEXT", 1.0)
    if status is not None:
        print(status.text)
    return 0 if result == "ACCEPTED" else 1


def main(argv):
    gs = os.environ.get("GS")
    port = os.environ.get("PORT")
    if not gs or not port:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        port = int(port)
    except ValueError:
        print(__doc__, file=sys.stderr)
        return 2

    if len(argv) == 1 and argv[0] == "list":
        cmd, args = "list", []
    elif len(argv) == 2 and argv[0] == "get":
        cmd, args = "get", [argv[1]]
    elif len(argv) == 3 and argv[0] == "set":
        cmd, args = "set", [argv[1], argv[2]]
    else:
        print(__doc__, file=sys.stderr)
        return 2

    # Deferred import (see the `mavutil = None` note above): the extended-
    # parameter messages are MAVLink 2 only, and pymavlink defaults to v1
    # unless told otherwise before the dialect is used.
    global mavutil
    os.environ["MAVLINK20"] = "1"
    from pymavlink import mavutil  # noqa: F811 (rebinds the module global)

    conn = mavutil.mavlink_connection("udpout:%s:%d" % (gs, port), source_system=255, source_component=190)
    conn.mav.heartbeat_send(
        mavutil.mavlink.MAV_TYPE_GCS,
        mavutil.mavlink.MAV_AUTOPILOT_INVALID,
        0,
        0,
        mavutil.mavlink.MAV_STATE_ACTIVE,
    )
    if not wait_for_drone(conn):
        print("no HEARTBEAT from the drone (system 1, component 191) within 5 s", file=sys.stderr)
        return 1

    if cmd == "list":
        return cmd_list(conn)
    if cmd == "get":
        return cmd_get(conn, args[0])
    return cmd_set(conn, args[0], args[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
