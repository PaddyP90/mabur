#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "config.h"
#include "mabur/config_params.h"

// Drone-side accessors and value coding for the MAVLink extended-parameter
// component (cfgparam::kTable, common/include/mabur/config_params.h). This
// file reads/writes mabur::Config fields and the MAVLink
// PARAM_EXT_* wire bytes; it never includes the MAVLink library itself.

namespace mabur::mavparam {

using cfgparam::Type;

// A decoded parameter value, one alternative live per cfgparam::Type.
struct Value {
  Type type;
  bool b = false;
  int32_t i = 0;
  float f = 0;
  std::string s;
};

// Reads row `idx` of cfgparam::kTable out of a loaded Config. `idx` must be
// < cfgparam::kCount.
Value read(const Config& cfg, size_t idx);

// Renders a Value as the TOML text for its row: "true"/"false", a decimal
// int, a float that always contains a '.' or 'e', or a string quoted with
// "...". Returns false (and leaves *out unspecified) for a string
// containing '"', '\', or a control character — render_toml never escapes.
bool render_toml(const Value& v, std::string* out);

// Decodes the 128-byte wire buffer for row `idx`, given the sender's
// MAV_PARAM_EXT_TYPE (as a plain number, see cfgparam::mav_type). False
// (the caller should answer PARAM_ACK_VALUE_UNSUPPORTED) when mav_type
// doesn't match the row's type, a Bool byte isn't 0 or 1, or a CUSTOM
// buffer has no NUL within 128 bytes or a non-printable byte before it.
bool decode_wire(size_t idx, uint8_t mav_type, const char wire[128], Value* out);

// Encodes `v` for PARAM_EXT_VALUE/PARAM_EXT_ACK: little-endian in bytes
// 0..3 for Bool/Int32/Real32, or NUL-terminated ASCII for Str. Zero-fills
// every byte past the value.
void encode_wire(const Value& v, char wire[128]);

}  // namespace mabur::mavparam
