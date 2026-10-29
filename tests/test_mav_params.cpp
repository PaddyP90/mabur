#include <cmath>
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

}  // namespace

TEST(reads_every_row_of_the_default_bundle) {
  Config c = load_config(bundle_path());
  CHECK(read(c, 0).f == 1.0f);            // radio.wall_margin_db
  CHECK(read(c, 1).b == true);            // radio.ldpc
  CHECK(read(c, 2).i == 60);              // venc.fps
  CHECK(read(c, 3).s == "1920x1080");     // venc.size
  CHECK(read(c, 4).i == 4);               // venc.slices
  CHECK(read(c, 5).i == 24000);           // encoder.bitrate_max_kbps
  CHECK(std::fabs(read(c, 6).f - 0.65f) < 1e-6f);
  CHECK(read(c, 7).b == true);            // record.enable
  CHECK(read(c, 8).i == 40000 && read(c, 9).i == 60 && read(c, 10).s == "1920x1080");
  CHECK(read(c, 11).b == false && read(c, 12).b == true);
}

TEST(rec_size_falls_back_to_venc_size_when_record_size_is_zero) {
  Config c = load_config(bundle_path());
  c.record.width = 0;
  c.record.height = 0;
  CHECK(read(c, 10).s == read(c, 3).s);
}

TEST(render_toml_forms) {
  std::string o;
  CHECK(render_toml(Value{Type::Bool, true}, &o) && o == "true");
  CHECK(render_toml(Value{Type::Int32, false, -5}, &o) && o == "-5");
  CHECK(render_toml(Value{Type::Real32, false, 0, 2.0f}, &o) && o == "2.0");
  CHECK(render_toml(Value{Type::Real32, false, 0, 0.65f}, &o) && o.find('.') != std::string::npos);
  CHECK(render_toml(Value{Type::Str, false, 0, 0, "1280x720"}, &o) && o == "\"1280x720\"");
  CHECK(!render_toml(Value{Type::Str, false, 0, 0, "a\"b"}, &o));
  CHECK(!render_toml(Value{Type::Str, false, 0, 0, "a\nb"}, &o));
}

TEST(decode_wire_rejects_type_mismatch_and_bad_bytes) {
  char w[128] = {};
  Value v;
  w[0] = 1; CHECK(decode_wire(1, 1, w, &v) && v.b);           // RADIO_LDPC UINT8 1
  w[0] = 2; CHECK(!decode_wire(1, 1, w, &v));                  // not 0/1
  CHECK(!decode_wire(1, 6, w, &v));                            // INT32 sent for a Bool row
  std::memset(w, 'x', 128); CHECK(!decode_wire(3, 11, w, &v)); // no NUL
  std::memcpy(w, "1280x720", 9); CHECK(decode_wire(3, 11, w, &v) && v.s == "1280x720");
  std::memcpy(w, "12\x01", 4); CHECK(!decode_wire(3, 11, w, &v));
  int32_t x = 90; std::memcpy(w, &x, 4); CHECK(decode_wire(2, 6, w, &v) && v.i == 90);
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
  // Int32: row 2 (VENC_FPS), MAV_PARAM_EXT_TYPE_INT32 == 6.
  {
    char w[128];
    Value v{Type::Int32, false, -1234};
    encode_wire(v, w);
    int32_t raw;
    std::memcpy(&raw, w, 4);
    CHECK(raw == -1234);
    for (int k = 4; k < 128; ++k) CHECK(w[k] == 0);
    Value out;
    CHECK(decode_wire(2, 6, w, &out) && out.i == -1234);
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
  // Str: row 3 (VENC_SIZE), MAV_PARAM_EXT_TYPE_CUSTOM == 11.
  {
    char w[128];
    Value v{Type::Str, false, 0, 0, "1920x1080"};
    encode_wire(v, w);
    CHECK(std::memcmp(w, "1920x1080", 9) == 0);
    CHECK(w[9] == '\0');
    for (int k = 10; k < 128; ++k) CHECK(w[k] == 0);
    Value out;
    CHECK(decode_wire(3, 11, w, &out) && out.s == "1920x1080");
  }
}

MTEST_MAIN
