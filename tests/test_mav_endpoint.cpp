// MavEndpoint: the maburgs MAVLink UDP endpoint, over real loopback sockets.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <vector>

#include "mtest.h"
#include "mabur/link_key.h"
#include "mabur/mav.h"
#include "mabur/rc_proto.h"
#include "mav_endpoint.h"
#include "mav_tunnel.h"

using namespace maburgs;
using Bytes = std::vector<uint8_t>;

namespace {

mabur::LinkKey test_key() { return *mabur::parse_key_hex("3f9a1c77e04b5d2290ab6ef1c8d34e5a"); }

Bytes hb_bytes() {
  mavlink_message_t msg;
  mavlink_msg_heartbeat_pack(255, 190, &msg, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0,
                             MAV_STATE_ACTIVE);
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  return Bytes(buf, buf + n);
}

// A plain blocking UDP socket on loopback standing in for a ground app.
struct Client {
  int fd = -1;
  Client() {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  }
  ~Client() { close_fd(); }
  void close_fd() {
    if (fd >= 0) ::close(fd);
    fd = -1;
  }
  bool send_to(int port, const Bytes& b) const {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sendto(fd, b.data(), b.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) ==
           static_cast<ssize_t>(b.size());
  }
  // One datagram, or empty after timeout_ms with nothing to read.
  Bytes recv(int timeout_ms) const {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) != 1) return {};
    uint8_t buf[2048];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    return n > 0 ? Bytes(buf, buf + n) : Bytes{};
  }
};

struct Rig {
  std::vector<Bytes> sent;
  MavTunnel tunnel{test_key(), [this](Bytes f) { sent.push_back(std::move(f)); },
                   [](const uint8_t*, size_t) {}};
  MavEndpoint ep;
  Rig() { tunnel.set_session(mabur::rc::TagCtx{11, 22, 0}); }
};

// Loopback datagrams are queued by the time sendto returns, but give the
// stack a moment rather than depend on that.
void poll_until(MavEndpoint& ep, MavTunnel& t, uint64_t want_packets) {
  for (int i = 0; i < 200 && t.stats().tx_packets < want_packets; ++i) {
    ep.poll(t);
    t.flush();
    if (t.stats().tx_packets < want_packets) usleep(1000);
  }
}

}  // namespace

TEST(open_binds_and_reports_the_kernel_chosen_port) {
  MavEndpoint ep;
  CHECK(!ep.ok());
  REQUIRE(ep.open("127.0.0.1", 0));
  CHECK(ep.ok());
  CHECK(ep.port() > 0);
  CHECK(!ep.has_peer());
}

TEST(open_refuses_a_bad_address) {
  MavEndpoint ep;
  CHECK(!ep.open("not-an-address", 0));
  CHECK(!ep.ok());
}

TEST(inbound_datagram_reaches_the_tunnel_and_sets_the_peer) {
  Rig r;
  REQUIRE(r.ep.open("127.0.0.1", 0));
  Client c;
  const Bytes hb = hb_bytes();
  REQUIRE(c.send_to(r.ep.port(), hb));
  poll_until(r.ep, r.tunnel, 1);
  CHECK(r.tunnel.stats().tx_packets == 1);
  CHECK(r.tunnel.stats().tx_frames == 1);
  REQUIRE(r.sent.size() == 1);
  auto m = mabur::rc::parse_mav(r.sent[0].data(), r.sent[0].size(), true);
  REQUIRE(m.has_value());
  CHECK(m->payload == hb);
  CHECK(r.ep.has_peer());
}

TEST(send_to_peer_delivers_one_datagram_to_the_last_sender) {
  Rig r;
  REQUIRE(r.ep.open("127.0.0.1", 0));
  Client a, b;
  const Bytes hb = hb_bytes();
  REQUIRE(a.send_to(r.ep.port(), hb));
  poll_until(r.ep, r.tunnel, 1);
  const Bytes reply = {0xFD, 1, 2, 3};
  r.ep.send_to_peer(reply.data(), reply.size());
  CHECK(a.recv(500) == reply);

  // b speaks last, so b is the peer now and a hears nothing more.
  REQUIRE(b.send_to(r.ep.port(), hb));
  poll_until(r.ep, r.tunnel, 2);
  r.ep.send_to_peer(reply.data(), reply.size());
  CHECK(b.recv(500) == reply);
  CHECK(a.recv(50).empty());
  CHECK(r.ep.dropped() == 0);
}

TEST(send_with_no_peer_is_dropped_and_counted) {
  MavEndpoint ep;
  REQUIRE(ep.open("127.0.0.1", 0));
  const Bytes b = {1, 2, 3};
  ep.send_to_peer(b.data(), b.size());
  CHECK(ep.dropped() == 1);
  MavEndpoint closed;   // never opened: same rule, no crash
  closed.send_to_peer(b.data(), b.size());
  CHECK(closed.dropped() == 1);
}

TEST(send_to_a_vanished_peer_never_blocks) {
  Rig r;
  REQUIRE(r.ep.open("127.0.0.1", 0));
  Client c;
  REQUIRE(c.send_to(r.ep.port(), hb_bytes()));
  poll_until(r.ep, r.tunnel, 1);
  REQUIRE(r.ep.has_peer());
  c.close_fd();
  const Bytes b(200, 0x55);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 2000; ++i) r.ep.send_to_peer(b.data(), b.size());
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  CHECK(ms < 1000);
  CHECK(r.ep.has_peer());
}

TEST(poll_with_nothing_pending_is_a_no_op) {
  Rig r;
  REQUIRE(r.ep.open("127.0.0.1", 0));
  r.ep.poll(r.tunnel);
  CHECK(r.tunnel.flush() == 0);
  CHECK(!r.ep.has_peer());
  MavEndpoint closed;
  closed.poll(r.tunnel);   // never opened: nothing to read
}

MTEST_MAIN
