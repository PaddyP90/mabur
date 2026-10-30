#include "param_client.h"

#include <utility>

#include "mabur/mav.h"

namespace maburgs {

namespace {

using mabur::cfgparam::kCount;

constexpr uint8_t kDroneSysId = 1;
constexpr uint8_t kDroneCompId = 191;  // MAV_COMP_ID_ONBOARD_COMPUTER

}  // namespace

ParamClient::ParamClient(SendFn send, uint8_t sysid, uint8_t compid)
    : send_(std::move(send)), sysid_(sysid), compid_(compid) {}

void ParamClient::send_list_request() {
  mavlink_message_t m;
  mavlink_msg_param_ext_request_list_pack(sysid_, compid_, &m, kDroneSysId, kDroneCompId);
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const uint16_t n = mavlink_msg_to_send_buffer(buf, &m);
  send_(buf, n);
  ++stats_.tx;
}

void ParamClient::send_read_request(size_t idx) {
  mavlink_message_t m;
  char id[16] = {};  // blank: param_index selects the row, not the id
  mavlink_msg_param_ext_request_read_pack(sysid_, compid_, &m, kDroneSysId, kDroneCompId, id,
                                          static_cast<int16_t>(idx));
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const uint16_t n = mavlink_msg_to_send_buffer(buf, &m);
  send_(buf, n);
  ++stats_.tx;
}

void ParamClient::set_linked(bool linked, uint64_t now_ms) {
  if (linked == state_.linked) return;
  state_ = ParamClientState{};
  state_.linked = linked;
  alive_cached_ = false;
  list_rounds_ = 0;
  list_last_req_ms_ = now_ms;
  if (linked) {
    state_.list = ParamListState::Pending;
    send_list_request();
  }
  ++seq_;
}

void ParamClient::on_packet(const uint8_t* p, size_t n, uint64_t now_ms) {
  // Fresh buffers every call: mavlink_frame_char_buffer carries no state
  // of its own beyond what's passed in, so a packet cut off mid-way can
  // never bleed into the next on_packet call.
  mavlink_message_t rx{}, out{};
  mavlink_status_t st{};
  bool ok = false;
  for (size_t i = 0; i < n; ++i) {
    if (mavlink_frame_char_buffer(&rx, &st, p[i], &out, nullptr) == MAVLINK_FRAMING_OK) ok = true;
  }
  if (!ok) {
    ++stats_.drop_bad;
    return;
  }
  ++stats_.rx;
  if (out.sysid != kDroneSysId || out.compid != kDroneCompId) return;

  bool changed = false;
  switch (out.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
      const bool was_alive = alive_cached_;
      state_.alive_ms = now_ms;
      alive_cached_ = true;
      if (!was_alive) changed = true;
      break;
    }
    case MAVLINK_MSG_ID_PARAM_EXT_VALUE: {
      mavlink_param_ext_value_t v;
      mavlink_msg_param_ext_value_decode(&out, &v);
      if (v.param_count != kCount && !state_.table_mismatch) {
        state_.table_mismatch = true;
        changed = true;
      }
      const int idx = mabur::cfgparam::find(v.param_id);
      if (idx < 0) {
        ++stats_.drop_unknown;
        break;
      }
      mabur::paramval::Value val;
      if (!mabur::paramval::decode_wire(static_cast<size_t>(idx), v.param_type, v.param_value,
                                        &val)) {
        ++stats_.drop_bad;
        break;
      }
      state_.rows[idx].value = val;
      state_.rows[idx].have_value = true;
      changed = true;
      if (state_.list == ParamListState::Pending || state_.list == ParamListState::Partial) {
        bool all = true;
        for (size_t i = 0; i < kCount; ++i) {
          if (!state_.rows[i].have_value) {
            all = false;
            break;
          }
        }
        if (all) state_.list = ParamListState::Complete;
      }
      break;
    }
    default:
      break;
  }
  if (changed) ++seq_;
}

void ParamClient::tick(uint64_t now_ms) {
  bool changed = false;
  if (alive_cached_ && !alive(now_ms)) {
    alive_cached_ = false;
    changed = true;
  }
  if (state_.list == ParamListState::Pending &&
      now_ms - list_last_req_ms_ >= kParamListRetryMs) {
    if (list_rounds_ >= kParamListRounds) {
      state_.list = ParamListState::Partial;
      changed = true;
    } else {
      bool any_missing = false;
      for (size_t i = 0; i < kCount; ++i) {
        if (!state_.rows[i].have_value) {
          send_read_request(i);
          any_missing = true;
        }
      }
      list_last_req_ms_ = now_ms;
      if (any_missing) {
        ++list_rounds_;
      } else {
        // Nothing left to re-read: the completion check in on_packet
        // should already have caught this, but a Complete transition
        // here too keeps tick() correct on its own.
        state_.list = ParamListState::Complete;
        changed = true;
      }
    }
  }
  if (changed) ++seq_;
}

void ParamClient::refresh(uint64_t now_ms) {
  if (!state_.linked) return;
  const bool changed = state_.list != ParamListState::Pending;
  state_.list = ParamListState::Pending;
  list_last_req_ms_ = now_ms;
  list_rounds_ = 0;
  send_list_request();
  if (changed) ++seq_;
}

bool ParamClient::set(size_t, const std::string&, uint64_t, std::string* why) {
  if (why) *why = "not implemented";
  return false;
}

bool ParamClient::alive(uint64_t now_ms) const {
  return state_.alive_ms != 0 && now_ms - state_.alive_ms <= kParamAliveMs;
}

}  // namespace maburgs
