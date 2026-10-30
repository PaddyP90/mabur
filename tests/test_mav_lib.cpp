#include <cstring>
#include "mtest.h"
#include "mabur/mav.h"
TEST(heartbeat_packs_and_parses) {
  mavlink_message_t msg; uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  mavlink_msg_heartbeat_pack(mabur::mav::kSysId, mabur::mav::kCompId, &msg,
      MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
  const uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  CHECK(buf[0] == 0xFD && n == 21);
  mavlink_message_t out; mavlink_status_t st{};
  bool got = false;
  for (uint16_t i = 0; i < n; ++i)
    if (mavlink_parse_char(mabur::mav::kChanGsRx, buf[i], &out, &st)) got = true;
  CHECK(got && out.msgid == MAVLINK_MSG_ID_HEARTBEAT && out.sysid == 1 && out.compid == 191);
}
TEST(param_ext_value_is_161_bytes_untruncated) {
  mavlink_message_t msg; uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  char value[128]; std::memset(value, 'x', sizeof value);
  mavlink_msg_param_ext_value_pack(1, 191, &msg, "ANY_PARAM_ID", value, MAV_PARAM_EXT_TYPE_INT32, 13, 2);
  CHECK(mavlink_msg_to_send_buffer(buf, &msg) == 161);
}
MTEST_MAIN
