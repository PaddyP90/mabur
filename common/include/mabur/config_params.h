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

struct Row {
  const char* id;       // the MAVLink parameter id (<=16 chars, no NUL guaranteed on the wire)
  const char* section;  // bundle/mabur.default.toml table
  const char* key;      // key within that table
  Type type;
  const char* help;     // one sentence, no trailing period
};

// bundle/mabur.default.toml keys exposed as drone parameters over MAVLink.
inline constexpr Row kTable[] = {
    {"RADIO_WALL_MARGN", "radio", "wall_margin_db", Type::Real32,
     "Safety margin below the calibrated TX-power walls; raise to lower TX power"},
    {"RADIO_LDPC", "radio", "ldpc", Type::Bool,
     "LDPC coding on every frame; turn off only for a ground card that cannot decode it"},
    {"VENC_FPS", "venc", "fps", Type::Int32,
     "Camera frame rate, which also selects the sensor's field-of-view mode"},
    {"VENC_SIZE", "venc", "size", Type::Str,
     "Encoder output resolution, must match the fps mode's aspect ratio"},
    {"VENC_SLICES", "venc", "slices", Type::Int32,
     "H.265 row slices on the link channel; 1 disables slice salvage"},
    {"ENC_BITRATE_MAX", "encoder", "bitrate_max_kbps", Type::Int32,
     "Ceiling on the encoder's bitrate at the top of the ladder"},
    {"ENC_AIRTIME", "encoder", "airtime_budget", Type::Real32,
     "Share of the link's real capacity the video may use"},
    {"REC_ENABLE", "record", "enable", Type::Bool,
     "Enable the onboard microSD recorder"},
    {"REC_BITRATE", "record", "bitrate_kbps", Type::Int32,
     "Constant bitrate of the onboard recording"},
    {"REC_FPS", "record", "fps", Type::Int32,
     "Onboard recording frame rate, at most the camera's fps"},
    {"REC_SIZE", "record", "size", Type::Str,
     "Onboard recording resolution, same aspect ratio as the camera's"},
    {"LOWPWR_ENABLE", "low_power", "enable", Type::Bool,
     "Lower bitrate and frame rate while disarmed to keep the VTX cool"},
    {"MSP_ENABLE", "msp", "enable", Type::Bool,
     "Flight-controller link for the OSD and arm state"},
};
constexpr size_t kCount = sizeof(kTable) / sizeof(kTable[0]);
constexpr int kMaxIdLen = 16;

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
