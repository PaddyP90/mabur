#pragma once
// MavTunnel — MAVLink packets over the RC link's T_MAV control frame, GS
// side. Shared by maburgs and the web GS core: pure data handling, the
// caller owns the sockets and the radio.
//
// Outbound (local MAVLink peer -> drone): push_outbound takes bytes cut at
// any boundary, a parser splits them into whole MAVLink 2 packets and
// queues each packet's wire bytes unchanged (re-serialising through the
// library would trim a non-truncating sender's zero tail while keeping its
// old CRC, corrupting the packet). flush() packs the queue greedily into
// as few T_MAV frames as fit kMavMaxPayload -- a packet never spans two
// frames -- and tags each with the session ctx and the next counter
// (TagCtx.seq32 = counter). The drone accepts only a counter above the
// last one it took this session, so the counter restarts at 1 whenever the
// session's vtx_nonce changes (a drone restart or re-rendezvous), the rule
// NackTracker::restart_counter follows. While the session is unlinked
// (vtx_nonce 0) outbound packets are dropped and counted: a ground app's
// heartbeats before link-up are noise, not something to replay later.
//
// Inbound (drone -> local peer): on_rc_frame takes an untagged T_MAV body,
// splits its payload and delivers each whole packet's wire bytes. A frame
// carrying the same drone counter as the last one taken is the other
// card's copy and is dropped (rx_dup); the counter memory clears when the
// session's vtx_nonce changes. A tagged T_MAV is the GS's own uplink heard
// by its other card (rx_uplink_heard), never a reject. The RC
// CRC already vouches for the bytes; the MAVLink CRC is the second check
// and is also what rejects a message id whose CRC_EXTRA the common dialect
// does not know. A partial packet at the end of a frame is dropped and the
// parser reset, so it never bleeds into the next frame.
//
// MAVLink 2 only: a v1 packet in either direction is dropped and counted.
//
// mabur/mav.h's helpers keep per-translation-unit channel state, so
// mav_tunnel.cpp is the GS's one TU that parses MAVLink. It parses on
// kChanGsLocal (outbound) and kChanGsRx (inbound); the channel state is
// process-wide, so one MavTunnel per process.
//
// Single-threaded: every call comes from the owner's core loop.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "mabur/link_key.h"
#include "mabur/rc_proto.h"

namespace maburgs {

struct MavTunnelStats {
  uint64_t tx_frames = 0, tx_packets = 0, rx_frames = 0, rx_packets = 0,
           drop_unlinked = 0,     // outbound packets with no session to tag them
           drop_bad_packet = 0,   // bad MAVLink CRC, v1, a partial or a T_MAV body that fails parse
           drop_oversize = 0,     // an outbound packet larger than kMavMaxPayload
           rx_dup = 0,            // a drone frame repeating the last counter (the other card's copy)
           rx_uplink_heard = 0;   // the GS's own tagged T_MAV, heard by its other card
};

class MavTunnel {
 public:
  using SendFn = std::function<void(std::vector<uint8_t> frame)>;   // a packed, tagged T_MAV body
  using DeliverFn = std::function<void(const uint8_t* mav_bytes, size_t n)>;  // one whole packet

  MavTunnel(mabur::LinkKey key, SendFn send, DeliverFn deliver);

  // The session the next frames are tagged under; ctx.seq32 is ignored
  // (the tunnel's counter fills it). vtx_nonce 0 = not linked: outbound is
  // dropped. A change of vtx_nonce restarts the counter: the next frame is 1,
  // and forgets the last inbound counter.
  void set_session(const mabur::rc::TagCtx& ctx);
  // Bytes from a local MAVLink peer, cut at any boundary. Whole packets are
  // queued (or dropped and counted while unlinked).
  void push_outbound(const uint8_t* bytes, size_t n);
  // Packs queued packets into as few T_MAV frames as fit kMavMaxPayload and
  // calls send for each. Returns frames sent.
  int flush();
  // A T_MAV frame body from the drone (the rc-sink hook). Splits it and
  // delivers each packet, unless it is a duplicate or the GS's own uplink.
  void on_rc_frame(const uint8_t* body, size_t len);

  const MavTunnelStats& stats() const { return stats_; }
  uint32_t counter() const { return counter_; }   // last counter used

 private:
  mabur::LinkKey key_;
  SendFn send_;
  DeliverFn deliver_;
  mabur::rc::TagCtx ctx_{};
  uint32_t vtx_seen_ = 0;
  uint32_t counter_ = 0;
  bool have_rx_ = false;   // rx_last_ holds a counter from this session
  uint32_t rx_last_ = 0;   // the drone counter of the last inbound frame taken
  std::deque<std::vector<uint8_t>> queue_;
  std::vector<uint8_t> out_acc_;   // wire bytes of the outbound packet being parsed
  std::vector<uint8_t> in_acc_;    // same, inbound
  MavTunnelStats stats_;
};

}  // namespace maburgs
