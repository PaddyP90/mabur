#include <cmath>
#include <cstring>
#include <string>

#include "mtest.h"
#include "mabur/param_value.h"

using namespace mabur::paramval;
using mabur::cfgparam::Type;

TEST(parse_text_by_type) {
  Value v;
  CHECK(parse_text(Type::Bool, "1", &v) && v.b);
  CHECK(parse_text(Type::Bool, "FALSE", &v) && !v.b);
  CHECK(!parse_text(Type::Bool, "2", &v));
  CHECK(parse_text(Type::Int32, "-5", &v) && v.i == -5);
  CHECK(!parse_text(Type::Int32, "5x", &v));
  CHECK(!parse_text(Type::Int32, "", &v));
  CHECK(!parse_text(Type::Int32, "3000000000", &v));
  CHECK(parse_text(Type::Real32, "0.65", &v) && std::fabs(v.f - 0.65f) < 1e-6f);
  CHECK(!parse_text(Type::Real32, "inf", &v));
  CHECK(!parse_text(Type::Real32, "1.0.0", &v));
  CHECK(parse_text(Type::Str, "1920x1080", &v) && v.s == "1920x1080");
  CHECK(!parse_text(Type::Str, "", &v));
  CHECK(!parse_text(Type::Str, "a\"b", &v));
  CHECK(!parse_text(Type::Str, std::string(128, 'a'), &v));
  CHECK(parse_text(Type::Str, std::string(127, 'a'), &v));
}

TEST(format_text_round_trips) {
  CHECK(format_text(Value{Type::Bool, true}) == "1");
  CHECK(format_text(Value{Type::Int32, false, 90}) == "90");
  CHECK(format_text(Value{Type::Real32, false, 0, 0.65f}) == "0.65");
  CHECK(format_text(Value{Type::Real32, false, 0, 2.0f}) == "2");
  CHECK(format_text(Value{Type::Str, false, 0, 0, "1728x816"}) == "1728x816");
}

TEST(wire_round_trip_every_type) {
  char w[128];
  Value v;
  encode_wire(Value{Type::Real32, false, 0, 0.65f}, w);
  CHECK(decode_wire(0, 9, w, &v) && v.f == 0.65f);
  encode_wire(Value{Type::Bool, true}, w);
  CHECK(decode_wire(1, 1, w, &v) && v.b);
  encode_wire(Value{Type::Int32, false, -7}, w);
  CHECK(decode_wire(4, 6, w, &v) && v.i == -7);
  encode_wire(Value{Type::Str, false, 0, 0, "1280x720"}, w);
  CHECK(decode_wire(9, 11, w, &v) && v.s == "1280x720");  // REC_SIZE
  // A buffer the Bool row (row 1) would itself accept (wire[0] == 1, rest
  // zero) — so the positive check pins row 1's mav_type to 1, and the
  // negative check below is rejected ONLY by the mav_type mismatch, not by
  // any byte-level content check.
  std::memset(w, 0, 128);
  w[0] = 1;
  CHECK(decode_wire(1, 1, w, &v) && v.b);
  CHECK(!decode_wire(1, 6, w, &v));  // type mismatch: Bool row, Int32 sent
}

MTEST_MAIN
