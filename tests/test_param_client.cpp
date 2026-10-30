// ParamClient: ground-side PARAM_EXT_* list/read/link state.
#include <cstring>
#include <vector>

#include "mabur/config_params.h"
#include "mabur/mav.h"
#include "mabur/param_value.h"
#include "mtest.h"
#include "param_client.h"

using namespace maburgs;
using Bytes = std::vector<uint8_t>;

static Bytes pkt(const mavlink_message_t& msg) {
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  return Bytes(buf, buf + n);
}

static Bytes value_pkt(const char* id, uint8_t type, const char wire[128], uint16_t count,
                        uint16_t index) {
  char id16[16] = {};
  std::strncpy(id16, id, 16);
  mavlink_message_t msg;
  mavlink_msg_param_ext_value_pack(1, 191, &msg, id16, wire, type, count, index);
  return pkt(msg);
}

static Bytes hb_pkt() {
  mavlink_message_t msg;
  mavlink_msg_heartbeat_pack(1, 191, &msg, MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID, 0,
                             0, MAV_STATE_ACTIVE);
  return pkt(msg);
}

static std::vector<mavlink_message_t> parse(const Bytes& b) {
  mavlink_reset_channel_status(MAVLINK_COMM_1);
  std::vector<mavlink_message_t> out;
  mavlink_message_t msg;
  mavlink_status_t status;
  for (uint8_t c : b) {
    if (mavlink_parse_char(MAVLINK_COMM_1, c, &msg, &status) == MAVLINK_FRAMING_OK) {
      out.push_back(msg);
    }
  }
  return out;
}

// A plausible value for row `idx`, matching its own type -- VENC_MODE gets
// a shape decode_wire's loader-shape check accepts, every other Str row
// gets a plain printable string.
static mabur::paramval::Value sample_value(size_t idx) {
  const auto& row = mabur::cfgparam::kTable[idx];
  mabur::paramval::Value v{row.type};
  switch (row.type) {
    case mabur::cfgparam::Type::Bool:
      v.b = true;
      break;
    case mabur::cfgparam::Type::Int32:
      v.i = 42;
      break;
    case mabur::cfgparam::Type::Real32:
      v.f = 1.5f;
      break;
    case mabur::cfgparam::Type::Str:
      v.s = (idx == mabur::cfgparam::kVencModeRow) ? "60@1920x1080" : "0x0";
      break;
  }
  return v;
}

// Builds and feeds a PARAM_EXT_VALUE for row `idx` with a plausible value.
static void feed_row(ParamClient& c, size_t idx, uint64_t now_ms,
                      uint16_t count = mabur::cfgparam::kCount) {
  const auto& row = mabur::cfgparam::kTable[idx];
  const mabur::paramval::Value v = sample_value(idx);
  char wire[128];
  mabur::paramval::encode_wire(v, wire);
  const Bytes bytes = value_pkt(row.id, mabur::cfgparam::mav_type(row.type), wire, count,
                                 static_cast<uint16_t>(idx));
  c.on_packet(bytes.data(), bytes.size(), now_ms);
}

struct Rig {
  std::vector<Bytes> sent;
  ParamClient c{[this](const uint8_t* p, size_t n) { sent.emplace_back(p, p + n); }};
};

TEST(link_up_requests_the_list_once) {
  Rig r;
  const uint32_t seq0 = r.c.seq();
  r.c.set_linked(true, 1000);
  REQUIRE(r.sent.size() == 1);
  auto msgs = parse(r.sent[0]);
  REQUIRE(msgs.size() == 1);
  CHECK(msgs[0].msgid == MAVLINK_MSG_ID_PARAM_EXT_REQUEST_LIST);
  mavlink_param_ext_request_list_t p;
  mavlink_msg_param_ext_request_list_decode(&msgs[0], &p);
  CHECK(p.target_system == 1 && p.target_component == 191);
  CHECK(r.c.state().list == ParamListState::Pending);
  CHECK(r.c.seq() == seq0 + 1);
}

TEST(twelve_values_complete_the_list) {
  Rig r;
  r.c.set_linked(true, 1000);
  for (size_t i = 0; i < mabur::cfgparam::kCount; ++i) feed_row(r.c, i, 1001);
  CHECK(r.c.state().list == ParamListState::Complete);
  for (size_t i = 0; i < mabur::cfgparam::kCount; ++i) CHECK(r.c.state().rows[i].have_value);
  CHECK(r.c.state().rows[mabur::cfgparam::kVencModeRow].value.s == "60@1920x1080");
  CHECK(!r.c.state().table_mismatch);
}

TEST(missing_index_is_reread_then_partial) {
  Rig r;
  r.c.set_linked(true, 1000);
  r.sent.clear();
  for (size_t i = 0; i < mabur::cfgparam::kCount; ++i) {
    if (i == 5) continue;
    feed_row(r.c, i, 1001);
  }
  REQUIRE(r.c.state().list == ParamListState::Pending);

  r.c.tick(3001);
  REQUIRE(r.sent.size() == 1);
  {
    auto msgs = parse(r.sent[0]);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].msgid == MAVLINK_MSG_ID_PARAM_EXT_REQUEST_READ);
    mavlink_param_ext_request_read_t p;
    mavlink_msg_param_ext_request_read_decode(&msgs[0], &p);
    CHECK(p.param_index == 5);
  }

  r.sent.clear();
  r.c.tick(5002);
  CHECK(r.sent.size() == 1);
  CHECK(r.c.state().list == ParamListState::Pending);

  r.sent.clear();
  r.c.tick(7003);
  CHECK(r.sent.size() == 1);
  CHECK(r.c.state().list == ParamListState::Pending);

  r.sent.clear();
  r.c.tick(9004);
  CHECK(r.sent.empty());
  CHECK(r.c.state().list == ParamListState::Partial);
}

TEST(list_with_wrong_count_flags_mismatch) {
  Rig r;
  r.c.set_linked(true, 1000);
  feed_row(r.c, mabur::cfgparam::kVencModeRow, 1001, /*count=*/13);
  CHECK(r.c.state().table_mismatch);
  CHECK(r.c.state().rows[mabur::cfgparam::kVencModeRow].have_value);
  CHECK(r.c.state().rows[mabur::cfgparam::kVencModeRow].value.s == "60@1920x1080");
}

TEST(unknown_id_is_dropped_and_counted) {
  Rig r;
  r.c.set_linked(true, 1000);
  char wire[128] = {};
  const Bytes bytes = value_pkt("NOPE", MAV_PARAM_EXT_TYPE_UINT8, wire,
                                 mabur::cfgparam::kCount, 0);
  r.c.on_packet(bytes.data(), bytes.size(), 1001);
  CHECK(r.c.stats().drop_unknown == 1);
  CHECK(!r.c.state().table_mismatch);
  for (size_t i = 0; i < mabur::cfgparam::kCount; ++i) CHECK(!r.c.state().rows[i].have_value);
}

TEST(refresh_relists_without_clearing) {
  Rig r;
  r.c.set_linked(true, 1000);
  for (size_t i = 0; i < mabur::cfgparam::kCount; ++i) feed_row(r.c, i, 1001);
  REQUIRE(r.c.state().list == ParamListState::Complete);

  r.sent.clear();
  r.c.refresh(2000);
  CHECK(r.c.state().list == ParamListState::Pending);
  for (size_t i = 0; i < mabur::cfgparam::kCount; ++i) CHECK(r.c.state().rows[i].have_value);
  REQUIRE(r.sent.size() == 1);
  auto msgs = parse(r.sent[0]);
  REQUIRE(msgs.size() == 1);
  CHECK(msgs[0].msgid == MAVLINK_MSG_ID_PARAM_EXT_REQUEST_LIST);
}

TEST(link_down_clears_everything) {
  Rig r;
  r.c.set_linked(true, 1000);
  for (size_t i = 0; i < mabur::cfgparam::kCount; ++i) feed_row(r.c, i, 1001);
  const uint32_t seq_before = r.c.seq();

  r.c.set_linked(false, 2000);
  CHECK(r.c.seq() == seq_before + 1);

  const ParamClientState def{};
  CHECK(r.c.state().linked == def.linked);
  CHECK(r.c.state().alive_ms == def.alive_ms);
  CHECK(r.c.state().table_mismatch == def.table_mismatch);
  CHECK(r.c.state().list == def.list);
  CHECK(r.c.state().pending_row == def.pending_row);
  for (size_t i = 0; i < mabur::cfgparam::kCount; ++i) {
    CHECK(r.c.state().rows[i].have_value == def.rows[i].have_value);
    CHECK(r.c.state().rows[i].st == def.rows[i].st);
  }
}

TEST(heartbeat_stamps_alive_and_flips_once) {
  Rig r;
  const Bytes hb = hb_pkt();
  const uint32_t seq0 = r.c.seq();

  r.c.on_packet(hb.data(), hb.size(), 1000);
  CHECK(r.c.alive(1000));
  CHECK(r.c.seq() == seq0 + 1);

  const uint32_t seq1 = r.c.seq();
  r.c.on_packet(hb.data(), hb.size(), 1500);
  CHECK(r.c.seq() == seq1);  // still alive: refreshing alive_ms alone doesn't bump

  r.c.tick(4600);
  CHECK(!r.c.alive(4600));
  CHECK(r.c.seq() == seq1 + 1);  // the timeout flip does bump
}

TEST(bad_packet_is_counted) {
  Rig r;
  const Bytes hb = hb_pkt();
  const Bytes truncated(hb.begin(), hb.begin() + hb.size() - 3);  // never reaches a CRC
  const uint32_t seq0 = r.c.seq();

  r.c.on_packet(truncated.data(), truncated.size(), 1000);
  CHECK(r.c.stats().drop_bad == 1);
  CHECK(r.c.stats().rx == 0);
  CHECK(!r.c.alive(1000));
  CHECK(r.c.seq() == seq0);
}

TEST(seq_is_stable_when_nothing_changes) {
  Rig r;
  const uint32_t seq0 = r.c.seq();
  r.c.tick(1000);
  r.c.tick(2000);
  CHECK(r.c.seq() == seq0);
}

MTEST_MAIN
