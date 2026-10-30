#include "mav_params.h"

#include <cmath>

namespace mabur::mavparam {

namespace {

std::string size_text(int width, int height) {
  return std::to_string(width) + "x" + std::to_string(height);
}

Value acc_wall_margin_db(const Config& c) {
  return Value{Type::Real32, false, 0, static_cast<float>(c.radio.wall_margin_db)};
}
Value acc_ldpc(const Config& c) { return Value{Type::Bool, c.radio.ldpc}; }
Value acc_venc_mode(const Config& c) {
  return Value{Type::Str, false, 0, 0,
               std::to_string(c.venc.core.fps) + "@" +
                   size_text(c.venc.core.width, c.venc.core.height)};
}
Value acc_venc_slices(const Config& c) {
  return Value{Type::Int32, false, static_cast<int32_t>(c.venc.core.slices)};
}
Value acc_bitrate_max_kbps(const Config& c) {
  return Value{Type::Int32, false, static_cast<int32_t>(c.encoder.bitrate_max_kbps)};
}
Value acc_airtime_budget(const Config& c) {
  return Value{Type::Real32, false, 0, static_cast<float>(c.encoder.airtime_budget)};
}
Value acc_rec_enable(const Config& c) { return Value{Type::Bool, c.record.enable}; }
Value acc_rec_bitrate_kbps(const Config& c) {
  return Value{Type::Int32, false, static_cast<int32_t>(c.record.bitrate_kbps)};
}
Value acc_rec_fps(const Config& c) {
  return Value{Type::Int32, false, static_cast<int32_t>(c.record.fps)};
}
// record.width/height 0x0 means "same as venc.size" (RecordCfg's own
// comment, drone/src/config.h) — fall back so a MAVLink reader always sees
// the size the recording will actually come out at, never a bare "0x0".
Value acc_rec_size(const Config& c) {
  int w = c.record.width, h = c.record.height;
  if (w == 0 && h == 0) {
    w = c.venc.core.width;
    h = c.venc.core.height;
  }
  return Value{Type::Str, false, 0, 0, size_text(w, h)};
}
Value acc_lowpwr_enable(const Config& c) { return Value{Type::Bool, c.low_power.enable}; }
Value acc_msp_enable(const Config& c) { return Value{Type::Bool, c.msp.enable}; }

using Accessor = Value (*)(const Config&);

// One accessor per row of cfgparam::kTable, same order. The static_assert
// below means a row added to that table without a matching accessor here
// fails the build instead of silently reading garbage at runtime.
constexpr Accessor kAccessors[] = {
    acc_wall_margin_db,    // RADIO_WALL_MARGN
    acc_ldpc,              // RADIO_LDPC
    acc_venc_mode,         // VENC_MODE
    acc_venc_slices,       // VENC_SLICES
    acc_bitrate_max_kbps,  // ENC_BITRATE_MAX
    acc_airtime_budget,    // ENC_AIRTIME
    acc_rec_enable,        // REC_ENABLE
    acc_rec_bitrate_kbps,  // REC_BITRATE
    acc_rec_fps,           // REC_FPS
    acc_rec_size,          // REC_SIZE
    acc_lowpwr_enable,     // LOWPWR_ENABLE
    acc_msp_enable,        // MSP_ENABLE
};
static_assert(sizeof(kAccessors) / sizeof(kAccessors[0]) == cfgparam::kCount,
              "mav_params accessor table must have one entry per cfgparam::kTable row");

bool is_control_or_quote(unsigned char c) {
  return c < 0x20 || c == 0x7f || c == '"' || c == '\\';
}

}  // namespace

bool read(const Config& cfg, size_t idx, Value* out) {
  if (idx >= cfgparam::kCount) return false;
  *out = kAccessors[idx](cfg);
  return true;
}

bool render_toml(const Value& v, std::string* out) {
  switch (v.type) {
    case Type::Bool:
      *out = v.b ? "true" : "false";
      return true;
    case Type::Int32:
      *out = std::to_string(v.i);
      return true;
    case Type::Real32: {
      // NaN/Inf have no TOML spelling %.9g would produce one anyway
      // ("nan", "inf") and the ".0" append below would mangle it further
      // ("inf.0") into something that doesn't even round-trip as a float.
      if (!std::isfinite(v.f)) return false;
      // paramval::float_text already picks the shortest text that reads
      // back as the same float; append ".0" when it picked the integer
      // form, so the TOML value is never read back as an Int32.
      std::string s = paramval::float_text(v.f);
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
          s.find('E') == std::string::npos) {
        s += ".0";
      }
      *out = s;
      return true;
    }
    case Type::Str: {
      for (unsigned char c : v.s) {
        if (is_control_or_quote(c)) return false;
      }
      *out = "\"" + v.s + "\"";
      return true;
    }
  }
  return false;
}

}  // namespace mabur::mavparam
