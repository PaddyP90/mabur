#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mtest.h"
#include "mav_params.h"
#include "config.h"

using namespace mabur;
using namespace mabur::mavparam;

namespace {

// Path to the committed default bundle config, resolved the same way
// tests/test_config_write.cpp's bundle_text() does.
std::string bundle_path() {
  return std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml";
}

// read() now returns bool (false for idx out of range); tests want the
// Value itself and REQUIRE success is unremarkable noise at every call
// site below, so wrap it once.
Value rd(const Config& c, size_t idx) {
  Value v;
  REQUIRE(read(c, idx, &v));
  return v;
}

}  // namespace

TEST(reads_every_row_of_the_default_bundle) {
  Config c = load_config(bundle_path());
  CHECK(rd(c, 0).f == 1.0f);            // radio.wall_margin_db
  CHECK(rd(c, 1).b == true);            // radio.ldpc
  CHECK(rd(c, 2).s == "60@1920x1080");  // venc.fps + venc.size
  CHECK(rd(c, 3).i == 4);               // venc.slices
  CHECK(rd(c, 4).i == 24000);           // encoder.bitrate_max_kbps
  CHECK(std::fabs(rd(c, 5).f - 0.65f) < 1e-6f);
  CHECK(rd(c, 6).b == true);            // record.enable
  CHECK(rd(c, 7).i == 40000 && rd(c, 8).i == 60 && rd(c, 9).s == "1920x1080");
  CHECK(rd(c, 10).b == false && rd(c, 11).b == true);
}

TEST(rec_size_falls_back_to_venc_mode_size_when_record_size_is_zero) {
  Config c = load_config(bundle_path());
  c.record.width = 0;
  c.record.height = 0;
  const std::string mode = rd(c, 2).s;
  CHECK(rd(c, 9).s == mode.substr(mode.find('@') + 1));
}

TEST(read_rejects_idx_out_of_range) {
  Config c = load_config(bundle_path());
  Value v;
  CHECK(!read(c, 12, &v));                    // cfgparam::kCount == 12
  CHECK(!read(c, static_cast<size_t>(-1), &v));
}

TEST(render_toml_forms) {
  std::string o;
  CHECK(render_toml(Value{Type::Bool, true}, &o) && o == "true");
  CHECK(render_toml(Value{Type::Int32, false, -5}, &o) && o == "-5");
  CHECK(render_toml(Value{Type::Real32, false, 0, 2.0f}, &o) && o == "2.0");
  CHECK(render_toml(Value{Type::Real32, false, 0, 0.65f}, &o) && o == "0.65");
  CHECK(render_toml(Value{Type::Real32, false, 0, 1.0f}, &o) && o == "1.0");
  CHECK(render_toml(Value{Type::Real32, false, 0, 1e-7f}, &o) && o == "1e-07");
  // A float that needs all nine digits still round-trips.
  const float awkward = 0.1f + 0.2f;
  CHECK(render_toml(Value{Type::Real32, false, 0, awkward}, &o) &&
        std::strtof(o.c_str(), nullptr) == awkward);
  CHECK(render_toml(Value{Type::Str, false, 0, 0, "1280x720"}, &o) && o == "\"1280x720\"");
  CHECK(!render_toml(Value{Type::Str, false, 0, 0, "a\"b"}, &o));
  CHECK(!render_toml(Value{Type::Str, false, 0, 0, "a\nb"}, &o));
  CHECK(!render_toml(Value{Type::Real32, false, 0, INFINITY}, &o));
  CHECK(!render_toml(Value{Type::Real32, false, 0, -INFINITY}, &o));
  CHECK(!render_toml(Value{Type::Real32, false, 0, NAN}, &o));
}

TEST(decode_wire_rejects_type_mismatch_and_bad_bytes) {
  char w[128] = {};
  Value v;
  w[0] = 1; CHECK(decode_wire(1, 1, w, &v) && v.b);           // RADIO_LDPC UINT8 1
  w[0] = 2; CHECK(!decode_wire(1, 1, w, &v));                  // not 0/1
  CHECK(!decode_wire(1, 6, w, &v));                            // INT32 sent for a Bool row
  std::memset(w, 'x', 128); CHECK(!decode_wire(9, 11, w, &v)); // no NUL (REC_SIZE)
  std::memcpy(w, "1280x720", 9); CHECK(decode_wire(9, 11, w, &v) && v.s == "1280x720");
  std::memcpy(w, "12\x01", 4); CHECK(!decode_wire(9, 11, w, &v));
  int32_t x = 90; std::memcpy(w, &x, 4); CHECK(decode_wire(3, 6, w, &v) && v.i == 90);  // VENC_SLICES
}

TEST(decode_wire_venc_mode_checks_the_shape) {
  char w[128]; Value v;
  std::memset(w, 0, 128); std::strcpy(w, "100@1728x972");
  CHECK(decode_wire(2, 11, w, &v) && v.s == "100@1728x972");
  std::strcpy(w, "100@1728");  CHECK(!decode_wire(2, 11, w, &v));
  std::strcpy(w, "1000@1x1");  CHECK(decode_wire(2, 11, w, &v));
}

TEST(decode_wire_rejects_non_finite_real32) {
  char w[128] = {};
  Value v;
  float nan_v = NAN;
  std::memcpy(w, &nan_v, 4);
  CHECK(!decode_wire(6, 9, w, &v));  // ENC_AIRTIME REAL32, NaN
  float inf_v = INFINITY;
  std::memcpy(w, &inf_v, 4);
  CHECK(!decode_wire(6, 9, w, &v));  // ENC_AIRTIME REAL32, +Inf
  float ninf_v = -INFINITY;
  std::memcpy(w, &ninf_v, 4);
  CHECK(!decode_wire(6, 9, w, &v));  // ENC_AIRTIME REAL32, -Inf
}

TEST(encode_wire_round_trips) {
  // Bool: row 1 (RADIO_LDPC), MAV_PARAM_EXT_TYPE_UINT8 == 1.
  {
    char w[128];
    Value v{Type::Bool, true};
    encode_wire(v, w);
    CHECK(static_cast<uint8_t>(w[0]) == 1);
    for (int k = 1; k < 128; ++k) CHECK(w[k] == 0);
    Value out;
    CHECK(decode_wire(1, 1, w, &out) && out.b == true);
  }
  // Int32: row 3 (VENC_SLICES), MAV_PARAM_EXT_TYPE_INT32 == 6.
  {
    char w[128];
    Value v{Type::Int32, false, -1234};
    encode_wire(v, w);
    int32_t raw;
    std::memcpy(&raw, w, 4);
    CHECK(raw == -1234);
    for (int k = 4; k < 128; ++k) CHECK(w[k] == 0);
    Value out;
    CHECK(decode_wire(3, 6, w, &out) && out.i == -1234);
  }
  // Real32: row 0 (RADIO_WALL_MARGN), MAV_PARAM_EXT_TYPE_REAL32 == 9.
  {
    char w[128];
    Value v{Type::Real32, false, 0, 1.5f};
    encode_wire(v, w);
    float raw;
    std::memcpy(&raw, w, 4);
    CHECK(raw == 1.5f);
    for (int k = 4; k < 128; ++k) CHECK(w[k] == 0);
    Value out;
    CHECK(decode_wire(0, 9, w, &out) && out.f == 1.5f);
  }
  // Str: row 9 (REC_SIZE), MAV_PARAM_EXT_TYPE_CUSTOM == 11.
  {
    char w[128];
    Value v{Type::Str, false, 0, 0, "1920x1080"};
    encode_wire(v, w);
    CHECK(std::memcmp(w, "1920x1080", 9) == 0);
    CHECK(w[9] == '\0');
    for (int k = 10; k < 128; ++k) CHECK(w[k] == 0);
    Value out;
    CHECK(decode_wire(9, 11, w, &out) && out.s == "1920x1080");
  }
}

MTEST_MAIN
