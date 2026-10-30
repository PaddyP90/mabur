#include "mtest.h"
#include "mabur/config_params.h"
#include <cstring>
#include <set>
#include <string>
using namespace mabur::cfgparam;
TEST(thirteen_rows_in_spec_order) {
  REQUIRE(kCount == 13);
  CHECK(!std::strcmp(kTable[0].id, "RADIO_WALL_MARGN") && !std::strcmp(kTable[0].section, "radio") && !std::strcmp(kTable[0].key, "wall_margin_db") && kTable[0].type == Type::Real32);
  CHECK(!std::strcmp(kTable[1].id, "RADIO_LDPC")     && kTable[1].type == Type::Bool);
  CHECK(!std::strcmp(kTable[2].id, "VENC_FPS")       && kTable[2].type == Type::Int32);
  CHECK(!std::strcmp(kTable[3].id, "VENC_SIZE")      && kTable[3].type == Type::Str);
  CHECK(!std::strcmp(kTable[4].id, "VENC_SLICES"));
  CHECK(!std::strcmp(kTable[5].id, "ENC_BITRATE_MAX") && !std::strcmp(kTable[5].section, "encoder") && !std::strcmp(kTable[5].key, "bitrate_max_kbps"));
  CHECK(!std::strcmp(kTable[6].id, "ENC_AIRTIME")    && kTable[6].type == Type::Real32);
  CHECK(!std::strcmp(kTable[7].id, "REC_ENABLE")     && !std::strcmp(kTable[7].section, "record"));
  CHECK(!std::strcmp(kTable[8].id, "REC_BITRATE"));
  CHECK(!std::strcmp(kTable[9].id, "REC_FPS")        && !std::strcmp(kTable[9].key, "fps"));
  CHECK(!std::strcmp(kTable[10].id, "REC_SIZE")      && kTable[10].type == Type::Str);
  CHECK(!std::strcmp(kTable[11].id, "LOWPWR_ENABLE") && !std::strcmp(kTable[11].section, "low_power"));
  CHECK(!std::strcmp(kTable[12].id, "MSP_ENABLE")    && !std::strcmp(kTable[12].section, "msp"));
}
TEST(ids_unique_and_at_most_16_chars_with_help) {
  std::set<std::string> ids;
  for (const Row& r : kTable) {
    CHECK(std::strlen(r.id) <= 16 && std::strlen(r.id) > 0);
    CHECK(ids.insert(r.id).second);
    CHECK(std::strlen(r.help) > 10);
  }
}
TEST(find_compares_at_most_16_bytes) {
  char wire[16]; std::memcpy(wire, "RADIO_WALL_MARGN", 16);     // exactly 16, no NUL
  CHECK(find(wire) == 0);
  CHECK(find("VENC_FPS") == 2);
  CHECK(find("NOPE") == -1);
}
TEST(mav_type_values) {
  CHECK(mav_type(Type::Bool) == 1 && mav_type(Type::Int32) == 6 && mav_type(Type::Real32) == 9 && mav_type(Type::Str) == 11);
}
TEST(apply_column_matches_the_mavlink_doc) {
  CHECK(kTable[0].apply == Apply::Reprogram && kTable[1].apply == Apply::Reprogram);   // RADIO_*
  CHECK(kTable[2].apply == Apply::Rebuild && kTable[3].apply == Apply::Rebuild && kTable[4].apply == Apply::Rebuild);
  CHECK(kTable[5].apply == Apply::Knob && kTable[6].apply == Apply::Knob);
  for (size_t i = 7; i <= 10; ++i) CHECK(kTable[i].apply == Apply::Rebuild);          // REC_*
  CHECK(kTable[11].apply == Apply::Knob && kTable[12].apply == Apply::Reprogram);
  CHECK(!std::strcmp(apply_name(Apply::Rebuild), "rebuild"));
}
MTEST_MAIN
