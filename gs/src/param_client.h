#pragma once
// ParamClient -- the ground-side MAVLink extended-parameter protocol
// client: list, read and link/heartbeat state against the drone's
// component (system 1, component 191). Pure data handling, driven only by
// a send callback, on_packet and tick(now); the caller owns the MAVLink
// tunnel and the clock. set() is declared here but not implemented yet
// (always refuses "not implemented").
//
// List. set_linked(true) clears every row, sets list = Pending and sends
// one PARAM_EXT_REQUEST_LIST. While Pending, every row still missing a
// value is re-read individually by PARAM_EXT_REQUEST_READ (param_index set,
// id left blank) once every kParamListRetryMs, for at most kParamListRounds
// rounds; list reaches Complete as soon as every row has a value (checked
// as values arrive, not only on tick) or Partial once the rounds run out
// with rows still missing. A PARAM_EXT_VALUE whose param_count disagrees
// with the table's own row count flags table_mismatch, but the value is
// still stored when its id is known; an id the table doesn't have is
// dropped and counted (drop_unknown). refresh() starts the same round
// again without clearing the rows already held. set_linked(false) drops
// every row back to the unlinked defaults.
//
// Heartbeat. A HEARTBEAT from 1/191 stamps alive_ms; alive() is true while
// now - alive_ms is within kParamAliveMs. A heartbeat that arrives while
// already alive only refreshes the timestamp and never bumps seq() on its
// own -- the flip from not-alive to alive (or the timeout flip back, caught
// in tick) is what counts.
//
// seq() bumps once per call (on_packet/tick/set_linked/refresh/set) that
// changed anything a caller could observe through state(), so a UI can
// poll it cheaply instead of diffing the whole struct.
//
// Parsing: each on_packet call gets one whole MAVLink packet (the MAVLink
// tunnel's deliver callback already splits the tunnel payload into
// packets) and parses it with its own local mavlink_message_t/
// mavlink_status_t, fresh every call -- see mabur/mav.h for why this
// never touches the tunnel's own channel state.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "mabur/config_params.h"
#include "mabur/param_value.h"

namespace maburgs {

enum class ParamRowState : uint8_t {
  Idle,
  Sending,
  InProgress,
  Accepted,
  Failed,
  Unsupported,
  Timeout
};

enum class ParamListState : uint8_t { None, Pending, Complete, Partial };

struct ParamRow {
  bool have_value = false;
  mabur::paramval::Value value;
  ParamRowState st = ParamRowState::Idle;
  std::string status;
  uint64_t result_ms = 0;
};

struct ParamClientState {
  bool linked = false;
  uint64_t alive_ms = 0;
  bool table_mismatch = false;
  ParamListState list = ParamListState::None;
  int pending_row = -1;
  ParamRow rows[mabur::cfgparam::kCount];
};

struct ParamClientStats {
  uint64_t tx = 0, rx = 0, drop_unknown = 0, drop_bad = 0, timeouts = 0;
};

constexpr uint64_t kParamListRetryMs = 2000, kParamSetRetryMs = 2000, kParamFinalMs = 15000,
                    kParamStatusMs = 1500, kParamAliveMs = 3000;
constexpr int kParamListRounds = 3, kParamSetRetries = 3;

class ParamClient {
 public:
  using SendFn = std::function<void(const uint8_t* pkt, size_t n)>;

  // sysid/compid: this client's own MAVLink identity in outgoing packets
  // (255/190, matching QGroundControl's convention -- the drone answers
  // whoever asks, see docs/mavlink.md "Ids").
  explicit ParamClient(SendFn send, uint8_t sysid = 255, uint8_t compid = 190);

  // The caller's own session link state, independent of the drone
  // heartbeat's alive(): true starts a fresh list round, false drops
  // every row back to the unlinked defaults. A call that repeats the
  // current value is a no-op.
  void set_linked(bool linked, uint64_t now_ms);
  // One whole MAVLink packet. Parses it statelessly; a packet from
  // anyone but system 1 / component 191 is ignored.
  void on_packet(const uint8_t* p, size_t n, uint64_t now_ms);
  // Drives the list's retry rounds and the alive() timeout. A no-op when
  // nothing is pending and alive() hasn't changed.
  void tick(uint64_t now_ms);
  // Starts a new list round without clearing the rows already held. A
  // no-op while unlinked.
  void refresh(uint64_t now_ms);
  // Not implemented yet: always returns false with *why = "not
  // implemented", and changes nothing.
  bool set(size_t row, const std::string& text, uint64_t now_ms, std::string* why);
  bool alive(uint64_t now_ms) const;
  const ParamClientState& state() const { return state_; }
  const ParamClientStats& stats() const { return stats_; }
  uint32_t seq() const { return seq_; }

 private:
  void send_list_request();
  void send_read_request(size_t idx);

  SendFn send_;
  uint8_t sysid_, compid_;
  ParamClientState state_;
  ParamClientStats stats_;
  uint32_t seq_ = 0;
  bool alive_cached_ = false;      // alive() as of the end of the last call, to detect a flip
  uint64_t list_last_req_ms_ = 0;  // when the list request, or the last retry round, went out
  int list_rounds_ = 0;            // retry rounds sent for the current list
};

}  // namespace maburgs
