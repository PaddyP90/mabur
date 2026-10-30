#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Schema for the drone config keys exposed as MAVLink extended parameters.
// Shared code: no MAVLink include here, so a future UI can compile this
// without the library. The MAV_PARAM_EXT_TYPE values this maps to are
// reproduced locally as plain integers below.

namespace mabur::cfgparam {

enum class Type : uint8_t { Bool, Int32, Real32, Str };

// How a set reaches the running process (drone/src/config_apply.cpp,
// apply_path_for): Knob is a live value RcAgent reads on its next tick,
// Reprogram changes a radio or peripheral setting in place, Rebuild stops
// and restarts the encoder pipeline in-process.
enum class Apply : uint8_t { Knob, Reprogram, Rebuild };

struct Row {
  const char* id;       // the MAVLink parameter id (<=16 chars, no NUL guaranteed on the wire)
  const char* section;  // bundle/mabur.default.toml table
  const char* key;      // key within that table
  Type type;
  Apply apply;
  const char* help;     // one sentence, no trailing period
};

// bundle/mabur.default.toml keys exposed as drone parameters over MAVLink.
inline constexpr Row kTable[] = {
    {"RADIO_WALL_MARGN", "radio", "wall_margin_db", Type::Real32, Apply::Reprogram,
     "Safety margin below the calibrated TX-power walls; raise to lower TX power"},  // the TX-power plan is recomputed in place
    {"RADIO_LDPC", "radio", "ldpc", Type::Bool, Apply::Reprogram,
     "LDPC coding on every frame; turn off only for a ground card that cannot decode it"},  // the radiotap flag
    {"VENC_FPS", "venc", "fps", Type::Int32, Apply::Rebuild,
     "Camera frame rate, which also selects the sensor's field-of-view mode"},
    {"VENC_SIZE", "venc", "size", Type::Str, Apply::Rebuild,
     "Encoder output resolution, must match the fps mode's aspect ratio"},
    {"VENC_SLICES", "venc", "slices", Type::Int32, Apply::Rebuild,
     "H.265 row slices on the link channel; 1 disables slice salvage"},
    {"ENC_BITRATE_MAX", "encoder", "bitrate_max_kbps", Type::Int32, Apply::Knob,
     "Bitrate ceiling, kbps"},
    {"ENC_AIRTIME", "encoder", "airtime_budget", Type::Real32, Apply::Knob,
     "Share of the link's real capacity the video may use"},
    {"REC_ENABLE", "record", "enable", Type::Bool, Apply::Rebuild,
     "Enable the onboard microSD recorder"},  // the recorder's encode channel is part of the pipeline
    {"REC_BITRATE", "record", "bitrate_kbps", Type::Int32, Apply::Rebuild,
     "Constant bitrate of the onboard recording"},
    {"REC_FPS", "record", "fps", Type::Int32, Apply::Rebuild,
     "Onboard recording frame rate, at most the camera's fps"},
    {"REC_SIZE", "record", "size", Type::Str, Apply::Rebuild,
     "Onboard recording resolution, same aspect ratio as the camera's"},
    {"LOWPWR_ENABLE", "low_power", "enable", Type::Bool, Apply::Knob,
     "Lower bitrate and frame rate while disarmed to keep the VTX cool; needs msp.enable for arm state"},
    {"MSP_ENABLE", "msp", "enable", Type::Bool, Apply::Reprogram,
     "Flight-controller link for the OSD and arm state"},  // the MSP thread starts or stops
};
inline constexpr size_t kCount = sizeof(kTable) / sizeof(kTable[0]);
inline constexpr int kMaxIdLen = 16;

// Index of the row with this id, or -1. `id16` is a wire-format field: only
// an id of the full 16 chars is guaranteed to lack a NUL, so strncmp's own
// stop-at-NUL behaviour keeps every comparison within the shorter buffer.
inline int find(const char* id16) {
  for (size_t i = 0; i < kCount; ++i) {
    if (std::strncmp(kTable[i].id, id16, kMaxIdLen) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// "knob" | "reprogram" | "rebuild": how docs/mavlink.md and a future UI
// label a row's apply path.
inline const char* apply_name(Apply a) {
  switch (a) {
    case Apply::Knob: return "knob";
    case Apply::Reprogram: return "reprogram";
    case Apply::Rebuild: return "rebuild";
  }
  return "?";
}

// MAV_PARAM_EXT_TYPE values this header maps to, reproduced as local
// constants so this file never has to include the MAVLink library.
inline uint8_t mav_type(Type t) {
  constexpr uint8_t kUint8 = 1;   // MAV_PARAM_EXT_TYPE_UINT8
  constexpr uint8_t kInt32 = 6;   // MAV_PARAM_EXT_TYPE_INT32
  constexpr uint8_t kReal32 = 9;  // MAV_PARAM_EXT_TYPE_REAL32
  constexpr uint8_t kCustom = 11; // MAV_PARAM_EXT_TYPE_CUSTOM
  switch (t) {
    case Type::Bool: return kUint8;
    case Type::Int32: return kInt32;
    case Type::Real32: return kReal32;
    case Type::Str: return kCustom;
  }
  return kCustom;
}

}  // namespace mabur::cfgparam
