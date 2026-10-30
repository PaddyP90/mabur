#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "config.h"
#include "mabur/config_params.h"
#include "mabur/param_value.h"

// Drone-side accessors for the MAVLink extended-parameter component
// (cfgparam::kTable, common/include/mabur/config_params.h). This file
// reads/writes mabur::Config fields; it never includes the MAVLink
// library itself. The value coding itself (Value, decode_wire, encode_wire)
// lives in mabur::paramval (common/include/mabur/param_value.h), shared
// with a ground-side client, and is re-exposed here under its old names.

namespace mabur::mavparam {

using cfgparam::Type;
using Value = paramval::Value;
using paramval::decode_wire;
using paramval::encode_wire;

// Reads row `idx` of cfgparam::kTable out of a loaded Config into *out.
// False, *out untouched, when idx >= cfgparam::kCount — callers that drive
// idx from a wire param_index must not be able to read past the accessor
// table.
bool read(const Config& cfg, size_t idx, Value* out);

// Renders a Value as the TOML text for its row: "true"/"false", a decimal
// int, a float in the fewest significant digits (6..9) that read back as
// the same float and always containing a '.' or 'e', or a string quoted with
// "...". Returns false (and leaves *out unspecified) for a string
// containing '"', '\', a control character (including DEL, 0x7f), or for
// a non-finite float (NaN/Inf have no TOML spelling render_toml will
// produce) — render_toml never escapes.
bool render_toml(const Value& v, std::string* out);

}  // namespace mabur::mavparam
