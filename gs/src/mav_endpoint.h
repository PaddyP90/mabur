#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "mav_tunnel.h"

namespace maburgs {

// The maburgs MAVLink UDP endpoint: one socket a ground app (or
// tools/bench/mavparam.py) talks MAVLink to, bridged onto the T_MAV tunnel.
//
// Last-peer, the rule a telemetry-radio bridge follows: the peer is whoever
// sent the most recent datagram (QGroundControl, MAVProxy and pymavlink all
// open with a heartbeat), and drone packets go to that address only. There
// is no session or handshake on this side; whoever can reach the socket is
// trusted, the same boundary as the calibration control socket. The
// over-the-air leg is authenticated by the tunnel's SipHash tag, so the
// bind address (loopback unless [mavlink] bind says otherwise) is what
// decides who may talk to the drone.
//
// Non-blocking and single-threaded: poll() and send_to_peer() run on the
// core loop. A send with no peer, or one the kernel refuses (a full socket
// buffer, an ICMP error from a vanished client), is dropped and counted --
// never retried, never waited on. MAVLink's own request/ack retries are the
// ground app's business.
class MavEndpoint {
 public:
  MavEndpoint() = default;
  ~MavEndpoint() { if (fd_ >= 0) close(fd_); }
  MavEndpoint(const MavEndpoint&) = delete;
  MavEndpoint& operator=(const MavEndpoint&) = delete;

  bool ok() const { return fd_ >= 0; }

  // Binds `bind` (a dotted IPv4 address) : `port`; port 0 lets the kernel
  // pick (tests read it back with port()).
  bool open(const std::string& bind, int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, bind.c_str(), &addr.sin_addr) != 1) return false;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close(fd);
      return false;
    }
    fd_ = fd;
    return true;
  }

  // The bound port (getsockname), 0 when not open.
  int port() const {
    if (fd_ < 0) return 0;
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
  }

  // Drains pending datagrams into tunnel.push_outbound and remembers the
  // last sender as the peer. At most kMaxPerPoll datagrams per call, so a
  // flooding client cannot hold the core loop; the rest wait for the next
  // tick in the socket buffer.
  void poll(MavTunnel& tunnel) {
    if (fd_ < 0) return;
    for (int i = 0; i < kMaxPerPoll; ++i) {
      sockaddr_in from{};
      socklen_t flen = sizeof(from);
      const ssize_t n = recvfrom(fd_, buf_, sizeof(buf_), 0,
                                 reinterpret_cast<sockaddr*>(&from), &flen);
      if (n < 0) return;   // drained (EAGAIN) or an error: nothing more this tick
      peer_ = from;
      have_peer_ = true;
      // An oversize datagram arrives truncated; the tunnel's parser drops
      // the cut packet and counts it.
      tunnel.push_outbound(buf_, static_cast<size_t>(n));
    }
  }

  // One datagram to the current peer. No peer, or a send the kernel
  // refuses, is dropped and counted.
  void send_to_peer(const uint8_t* bytes, size_t n) {
    if (fd_ < 0 || !have_peer_) { ++dropped_; return; }
    const ssize_t r = sendto(fd_, bytes, n, MSG_DONTWAIT,
                             reinterpret_cast<const sockaddr*>(&peer_), sizeof(peer_));
    if (r != static_cast<ssize_t>(n)) ++dropped_;
  }

  bool has_peer() const { return have_peer_; }
  // Datagrams to the peer that were never sent (no peer, or sendto failed).
  uint64_t dropped() const { return dropped_; }

  static constexpr int kMaxPerPoll = 64;

 private:
  int fd_ = -1;
  sockaddr_in peer_{};
  bool have_peer_ = false;
  uint64_t dropped_ = 0;
  // Larger than any MAVLink 2 packet (280 B); a datagram may carry several.
  uint8_t buf_[4096];
};

}  // namespace maburgs
