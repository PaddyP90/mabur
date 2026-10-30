#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mabur/raw_dvr.h"
#include "mtest.h"

using mabur::RawDvr;

namespace {

std::string scratch(const char* name) { return std::string(MABUR_TEST_SCRATCH_DIR) + "/" + name; }

bool exists(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return f.good();
}

std::vector<uint8_t> read_whole_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- minimal box walker (mirrors tests/test_dvr_mux.cpp's) --------------
struct Box {
  std::string type;
  size_t off, size;
  size_t payload_off() const { return off + 8; }
};

uint32_t read_u32(const std::vector<uint8_t>& f, size_t p) {
  return (static_cast<uint32_t>(f[p]) << 24) | (static_cast<uint32_t>(f[p + 1]) << 16) |
         (static_cast<uint32_t>(f[p + 2]) << 8) | static_cast<uint32_t>(f[p + 3]);
}

uint16_t read_u16(const std::vector<uint8_t>& f, size_t p) {
  return static_cast<uint16_t>((f[p] << 8) | f[p + 1]);
}

std::vector<Box> parse_boxes(const std::vector<uint8_t>& f, size_t begin, size_t end) {
  std::vector<Box> out;
  size_t p = begin;
  while (p + 8 <= end) {
    const uint32_t sz = read_u32(f, p);
    if (sz < 8 || p + sz > end) break;
    out.push_back(Box{std::string(reinterpret_cast<const char*>(&f[p + 4]), 4), p, sz});
    p += sz;
  }
  return out;
}

const Box* find(const std::vector<Box>& boxes, const char* type) {
  for (const Box& b : boxes)
    if (b.type == type) return &b;
  return nullptr;
}

// Walks moov/trak/mdia/minf/stbl/stsd down to the hvc1 VisualSampleEntry and
// reads its width/height fields -- the coded size the moov was written with.
std::pair<int, int> moov_size_of(const std::string& path) {
  const std::vector<uint8_t> f = read_whole_file(path);
  const std::vector<Box> top = parse_boxes(f, 0, f.size());
  const Box* moov = find(top, "moov");
  if (!moov) return {0, 0};
  const Box* trak = find(parse_boxes(f, moov->payload_off(), moov->off + moov->size), "trak");
  if (!trak) return {0, 0};
  const Box* mdia = find(parse_boxes(f, trak->payload_off(), trak->off + trak->size), "mdia");
  if (!mdia) return {0, 0};
  const Box* minf = find(parse_boxes(f, mdia->payload_off(), mdia->off + mdia->size), "minf");
  if (!minf) return {0, 0};
  const Box* stbl = find(parse_boxes(f, minf->payload_off(), minf->off + minf->size), "stbl");
  if (!stbl) return {0, 0};
  const Box* stsd = find(parse_boxes(f, stbl->payload_off(), stbl->off + stbl->size), "stsd");
  if (!stsd) return {0, 0};
  // stsd is a FullBox with an entry_count before its entries.
  const Box* hvc1 = find(parse_boxes(f, stsd->payload_off() + 8, stsd->off + stsd->size), "hvc1");
  if (!hvc1) return {0, 0};
  // SampleEntry(8) + VisualSampleEntry fixed fields up to width/height at 24/26.
  return {read_u16(f, hvc1->payload_off() + 24), read_u16(f, hvc1->payload_off() + 26)};
}

// Total sample count across every moof/traf/trun in the file.
int samples_in(const std::string& path) {
  const std::vector<uint8_t> f = read_whole_file(path);
  int total = 0;
  for (const Box& b : parse_boxes(f, 0, f.size())) {
    if (b.type != "moof") continue;
    const Box* traf = find(parse_boxes(f, b.payload_off(), b.off + b.size), "traf");
    if (!traf) continue;
    const Box* trun = find(parse_boxes(f, traf->payload_off(), traf->off + traf->size), "trun");
    if (!trun) continue;
    total += static_cast<int>(read_u32(f, trun->payload_off() + 4));
  }
  return total;
}

void nal(std::vector<uint8_t>& au, std::initializer_list<uint8_t> bytes) {
  au.insert(au.end(), {0, 0, 0, 1});
  au.insert(au.end(), bytes);
}

// A GDR refresh start: VPS+SPS+PPS + a TRAIL_R slice (never an IRAP).
// SPS = the 720p x265 SPS from test_hevc_params.cpp, so dimensions parse.
std::vector<uint8_t> param_au(uint8_t tag) {
  std::vector<uint8_t> au;
  nal(au, {0x40, 0x01, 0x0c, 0x01, 0xff, 0xff});  // VPS (content irrelevant to RawDvr)
  au.insert(au.end(), {0, 0, 1});
  const uint8_t sps[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
                         0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xa0, 0x02, 0x80, 0x80, 0x2d, 0x16,
                         0x59, 0x59, 0xa4, 0x93, 0x2b, 0xc0, 0x5a, 0x02, 0x00, 0x00, 0x03, 0x00,
                         0x02, 0x00, 0x00, 0x03, 0x00, 0x78, 0x10};
  au.insert(au.end(), std::begin(sps), std::end(sps));
  nal(au, {0x44, 0x01, 0xc1, 0x72});              // PPS
  nal(au, {0x02, 0x01, tag, tag});                // TRAIL_R
  return au;
}

// A real IDR: VPS+SPS+PPS + an IDR_W_RADL slice. The only AU a recording
// may begin on, and the only sync sample.
std::vector<uint8_t> idr_au(uint8_t tag) {
  std::vector<uint8_t> au = param_au(tag);
  au[au.size() - 4] = 0x26;                       // TRAIL_R slice -> IDR_W_RADL
  return au;
}

// Named for the size-change tests below: param_au()/idr_au() already encode
// 1280x720 (checked against HevcParams::sps_dimensions in
// size_change_at_irap_rolls_to_the_next_sink).
std::vector<uint8_t> idr_au_720(uint8_t tag) { return idr_au(tag); }

// Same PPS content as param_au()/idr_au(), at a different coded size (the
// vendored 1920x1080 x265 SPS from test_hevc_params.cpp's kSps1080).
std::vector<uint8_t> param_au_1080(uint8_t tag) {
  std::vector<uint8_t> au;
  nal(au, {0x40, 0x01, 0x0c, 0x01, 0xff, 0xff});  // VPS (content irrelevant to RawDvr)
  au.insert(au.end(), {0, 0, 1});
  const uint8_t sps[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
                         0x03, 0x00, 0x00, 0x03, 0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5,
                         0x96, 0x56, 0x69, 0x24, 0xca, 0xf0, 0x16, 0x80, 0x80, 0x00, 0x00, 0x03,
                         0x00, 0x80, 0x00, 0x00, 0x1e, 0x04};
  au.insert(au.end(), std::begin(sps), std::end(sps));
  nal(au, {0x44, 0x01, 0xc1, 0x72});              // PPS
  nal(au, {0x02, 0x01, tag, tag});                // TRAIL_R
  return au;
}

std::vector<uint8_t> idr_au_1080(uint8_t tag) {
  std::vector<uint8_t> au = param_au_1080(tag);
  au[au.size() - 4] = 0x26;                       // TRAIL_R slice -> IDR_W_RADL
  return au;
}

// Same size as idr_au_720(), different PPS bytes (the GDR encoder rewrites
// parameter sets on every refresh start): must never roll on its own.
std::vector<uint8_t> idr_au_720_other_pps(uint8_t tag) {
  std::vector<uint8_t> au;
  nal(au, {0x40, 0x01, 0x0c, 0x01, 0xff, 0xff});  // VPS
  au.insert(au.end(), {0, 0, 1});
  const uint8_t sps[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
                         0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xa0, 0x02, 0x80, 0x80, 0x2d, 0x16,
                         0x59, 0x59, 0xa4, 0x93, 0x2b, 0xc0, 0x5a, 0x02, 0x00, 0x00, 0x03, 0x00,
                         0x02, 0x00, 0x00, 0x03, 0x00, 0x78, 0x10};
  au.insert(au.end(), std::begin(sps), std::end(sps));
  nal(au, {0x44, 0x01, 0xc1, 0x73});              // PPS, differs from idr_au_720's last byte
  nal(au, {0x26, 0x01, tag, tag});                // IDR_W_RADL
  return au;
}

std::vector<uint8_t> p_au(uint8_t tag) {
  std::vector<uint8_t> au;
  nal(au, {0x02, 0x01, tag, tag, tag});
  return au;
}

void feed(RawDvr& d, const std::vector<uint8_t>& au, uint32_t pts, bool complete = true) {
  d.feed(au.data(), au.size(), pts, complete);
}

}  // namespace

TEST(param_set_detection) {
  const auto k = param_au(1), p = p_au(2);
  CHECK(mabur::au_has_param_sets(k.data(), k.size()));
  CHECK(!mabur::au_has_param_sets(p.data(), p.size()));
}

TEST(irap_detection) {
  const auto i = idr_au(1), g = param_au(2), p = p_au(3);
  CHECK(mabur::au_is_irap(i.data(), i.size()));
  CHECK(!mabur::au_is_irap(g.data(), g.size()));   // GDR refresh start: a P slice
  CHECK(!mabur::au_is_irap(p.data(), p.size()));
}

// The live encoder is GDR: VPS/SPS/PPS ride on a TRAIL_R refresh start every
// gop_s, real IDRs are rare. A file that begins on the refresh start has no
// IRAP up front, and Apple's decoder (QuickTime, VLC on macOS) refuses it --
// as a web-GS recording did. Only an IDR opens.
TEST(nothing_written_before_first_idr) {
  const std::string path = scratch("rawdvr_wait.mp4");
  std::remove(path.c_str());
  RawDvr d;
  d.start(path, 0, 0);
  CHECK(d.state() == RawDvr::State::WaitSync);
  feed(d, p_au(1), 0);
  feed(d, p_au(2), 16667);
  feed(d, param_au(3), 33334);                   // refresh start: not an IDR
  CHECK(d.state() == RawDvr::State::WaitSync);
  CHECK(!exists(path));
  feed(d, idr_au(4), 50001);
  CHECK(d.state() == RawDvr::State::Recording);
  CHECK(exists(path));
  feed(d, p_au(5), 66668);
  d.stop();
  CHECK(d.state() == RawDvr::State::Off);
  CHECK(d.samples() == 2);
  CHECK(d.err() == RawDvr::Err::None);
}

TEST(truncated_aus_are_skipped_even_idrs) {
  const std::string path = scratch("rawdvr_trunc.mp4");
  std::remove(path.c_str());
  RawDvr d;
  d.start(path, 0, 0);
  feed(d, idr_au(1), 0, /*complete=*/false);
  CHECK(d.state() == RawDvr::State::WaitSync);   // a truncated refresh never opens
  feed(d, idr_au(2), 16667);
  feed(d, p_au(3), 33334, false);
  feed(d, p_au(4), 50001);
  d.stop();
  CHECK(d.samples() == 2);
}

// Only IDR AUs are sync samples: base P AUs (old "sid 0") must not
// cut fragments. 5 P AUs inside one 1 s fragment window -> 1 fragment.
TEST(plain_aus_do_not_cut_fragments) {
  const std::string path = scratch("rawdvr_frag.mp4");
  std::remove(path.c_str());
  RawDvr d;
  d.start(path, 0, 0, 1000);
  feed(d, idr_au(1), 0);
  for (uint32_t i = 1; i <= 5; ++i) feed(d, p_au(static_cast<uint8_t>(i)), i * 16667);
  d.stop();
  CHECK(d.samples() == 6);
  CHECK(d.fragments() == 1);
}

// A refresh start mid-recording is an ordinary sample: marking it sync told
// players it was a random-access point it is not (702 of 844 "sync" samples in
// one web-GS recording were P slices).
TEST(gdr_refresh_start_is_not_a_sync_point) {
  const std::string path = scratch("rawdvr_gdr.mp4");
  std::remove(path.c_str());
  RawDvr d;
  d.start(path, 0, 0, 1000);
  feed(d, idr_au(1), 0);
  feed(d, p_au(2), 16667);
  feed(d, param_au(3), 33334);                   // must not cut
  feed(d, p_au(4), 50001);
  feed(d, idr_au(5), 66668);                     // cuts
  d.stop();
  CHECK(d.samples() == 5);
  CHECK(d.fragments() == 2);
}

TEST(second_start_waits_for_next_idr) {
  const std::string a = scratch("rawdvr_a.mp4"), b = scratch("rawdvr_b.mp4");
  std::remove(a.c_str());
  std::remove(b.c_str());
  RawDvr d;
  d.start(a, 0, 0);
  feed(d, idr_au(1), 0);
  feed(d, p_au(2), 16667);
  d.stop();
  d.start(b, 0, 0);                 // params are complete (sticky) already
  feed(d, p_au(3), 33334);          // must NOT open on this P AU
  feed(d, param_au(5), 41000);      // nor on a refresh start
  CHECK(d.state() == RawDvr::State::WaitSync);
  CHECK(!exists(b));
  feed(d, idr_au(4), 50001);
  CHECK(d.state() == RawDvr::State::Recording);
  d.stop();
  CHECK(d.samples() == 1);
}

TEST(stop_before_sync_writes_nothing) {
  const std::string path = scratch("rawdvr_early.mp4");
  std::remove(path.c_str());
  RawDvr d;
  d.start(path, 0, 0);
  feed(d, p_au(1), 0);
  d.stop();
  CHECK(d.state() == RawDvr::State::Off);
  CHECK(!d.opened());
  CHECK(!exists(path));
}

TEST(open_failure_is_error_and_ignores_feed) {
  RawDvr d;
  d.start(scratch("no_such_dir/x.mp4"), 0, 0);
  feed(d, idr_au(1), 0);
  CHECK(d.state() == RawDvr::State::Error);
  CHECK(d.err() == RawDvr::Err::Open);
  CHECK(!d.opened());
  feed(d, idr_au(2), 16667);          // ignored, no crash
  CHECK(d.state() == RawDvr::State::Error);
  d.stop();                             // clears to Off, err kept for reporting
  CHECK(d.state() == RawDvr::State::Off);
  CHECK(d.err() == RawDvr::Err::Open);
}

// ---- sink path (the web GS's OPFS file goes through this) ----
struct MemSink : mabur::DvrSink {
  std::vector<uint8_t>* out;
  bool* destroyed;
  int writes_left;   // fail every write after this many (-1 = never)
  int* calls;        // optional: bumped on every write() invocation, success or not
  MemSink(std::vector<uint8_t>* o, bool* d, int left = -1, int* c = nullptr)
      : out(o), destroyed(d), writes_left(left), calls(c) {}
  ~MemSink() override { if (destroyed) *destroyed = true; }
  bool write(const uint8_t* p, size_t n) override {
    if (calls) ++*calls;
    if (writes_left == 0) return false;
    if (writes_left > 0) --writes_left;
    out->insert(out->end(), p, p + n);
    return true;
  }
  bool flush() override { return true; }
};

std::vector<uint8_t> read_all(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

TEST(sink_output_matches_file_output) {
  const std::string path = scratch("rawdvr_same.mp4");
  std::remove(path.c_str());
  std::vector<uint8_t> mem;
  RawDvr a, b;
  a.start(path, 0, 0);
  b.start(std::make_unique<MemSink>(&mem, nullptr), 0, 0);
  const std::vector<std::vector<uint8_t>> aus = {p_au(1), idr_au(2), p_au(3), p_au(4), idr_au(5), p_au(6)};
  uint32_t pts = 0;
  for (const auto& au : aus) { feed(a, au, pts); feed(b, au, pts); pts += 16667; }
  a.stop();
  b.stop();
  CHECK(!mem.empty());
  CHECK(read_all(path) == mem);
  CHECK(b.bytes() == mem.size());
}

TEST(null_sink_is_open_error) {
  RawDvr d;
  d.start(std::unique_ptr<mabur::DvrSink>{}, 0, 0);
  CHECK(d.state() == RawDvr::State::Error);
  CHECK(d.err() == RawDvr::Err::Open);
  feed(d, idr_au(1), 0);               // ignored
  CHECK(d.state() == RawDvr::State::Error);
}

// Like a full OPFS quota: the init segment lands, the first fragment write fails.
TEST(write_failure_is_sticky_error) {
  std::vector<uint8_t> mem;
  bool destroyed = false;
  RawDvr d;
  d.start(std::make_unique<MemSink>(&mem, &destroyed, 1), 0, 0);
  feed(d, idr_au(1), 0);               // opens: init segment = write #1
  CHECK(d.state() == RawDvr::State::Recording);
  feed(d, p_au(2), 16667);
  feed(d, idr_au(3), 33334);           // key: cuts the first fragment -> write fails
  CHECK(d.state() == RawDvr::State::Error);
  CHECK(d.err() == RawDvr::Err::Write);
  CHECK(destroyed);                      // sink closed, what was written stays
  CHECK(d.bytes() > 0);
  feed(d, p_au(4), 50001);               // ignored
  CHECK(d.state() == RawDvr::State::Error);
}

// Once a fragment's write starts failing, DvrMux must not keep issuing more
// sink writes for the rest of that fragment, nor from the close() a failed
// write triggers.
TEST(write_failure_stops_further_sink_writes) {
  std::vector<uint8_t> mem;
  bool destroyed = false;
  int calls = 0;
  RawDvr d;
  // 2 successful writes: the init segment (moov, on open) and the cut
  // fragment's moof/mdat header. Every write from the first pending sample
  // onward must be refused -- and, with the fix, never even attempted.
  d.start(std::make_unique<MemSink>(&mem, &destroyed, 2, &calls), 0, 0);
  feed(d, idr_au(1), 0);       // opens: write #1 (moov)
  feed(d, p_au(2), 16667);       // buffered: not a cut
  feed(d, p_au(3), 33334);       // buffered
  feed(d, idr_au(4), 50001);   // key: cuts -> header write #2 ok, then sample writes fail
  CHECK(d.state() == RawDvr::State::Error);
  CHECK(d.err() == RawDvr::Err::Write);
  CHECK(calls == 3);             // moov, moof/mdat header, ONE failed sample write -- no more
  const size_t bytes_after_failure = mem.size();
  feed(d, p_au(5), 66668);       // ignored: state is already Error
  d.stop();
  CHECK(calls == 3);             // stop()'s close() must not touch the dead sink again
  CHECK(mem.size() == bytes_after_failure);
}

TEST(unopened_sink_is_dropped_unwritten_on_stop) {
  std::vector<uint8_t> mem;
  bool destroyed = false;
  RawDvr d;
  d.start(std::make_unique<MemSink>(&mem, &destroyed), 0, 0);
  feed(d, p_au(1), 0);                   // before any sync point
  d.stop();
  CHECK(destroyed);
  CHECK(mem.empty());
  CHECK(d.bytes() == 0);
  CHECK(!d.opened());
}

TEST(start_while_recording_seals_previous_file) {
  const std::string a = scratch("rawdvr_c.mp4"), b = scratch("rawdvr_d.mp4");
  std::remove(a.c_str());
  std::remove(b.c_str());
  RawDvr d;
  d.start(a, 0, 0);
  feed(d, idr_au(1), 0);
  d.start(b, 0, 0);
  CHECK(d.state() == RawDvr::State::WaitSync);
  CHECK(d.err() == RawDvr::Err::None);
  std::ifstream f(a, std::ios::binary | std::ios::ate);
  CHECK(f.good() && f.tellg() > 0);     // a was flushed and closed
}

// ---- roll on a coded-size change -----------------------------------------

TEST(size_change_at_irap_rolls_to_the_next_sink) {
  // Confirm the vendored SPS bytes parse to the sizes the rest of this test
  // relies on, rather than trusting the comment above param_au_1080().
  {
    mabur::HevcParams hp720;
    const auto a720 = param_au(1);
    hp720.feed(a720.data(), a720.size());
    int w = 0, h = 0;
    REQUIRE(hp720.sps_dimensions(&w, &h));
    REQUIRE(w == 1280 && h == 720);

    mabur::HevcParams hp1080;
    const auto a1080 = param_au_1080(1);
    hp1080.feed(a1080.data(), a1080.size());
    REQUIRE(hp1080.sps_dimensions(&w, &h));
    REQUIRE(w == 1920 && h == 1080);
  }

  const std::string roll1 = scratch("roll1.mp4"), roll2 = scratch("roll2.mp4");
  std::remove(roll1.c_str());
  std::remove(roll2.c_str());
  RawDvr d;
  int calls = 0;
  d.set_next_sink([&](std::string* name) {
    ++calls;
    *name = roll2;
    return mabur::open_file_sink(*name);
  });
  d.start(roll1, 0, 0);
  feed(d, idr_au_720(1), 0);
  feed(d, p_au(2), 16667);
  feed(d, idr_au_1080(3), 33333);       // size changes: rolls before this sample is written
  feed(d, p_au(4), 50000);
  CHECK(calls == 1);
  CHECK(d.rolls() == 1);
  CHECK(d.path() == roll2);
  // last_sealed_bytes() must count the WHOLE sealed file, including the
  // fragment that was still pending when the roll closed it (read after
  // close(), not before -- an earlier version undercounted by exactly that
  // fragment).
  const uint64_t sealed_bytes = d.last_sealed_bytes();
  d.stop();
  CHECK(exists(roll1));
  CHECK(exists(roll2));
  CHECK(sealed_bytes == read_whole_file(roll1).size());
  CHECK(moov_size_of(roll1) == std::make_pair(1280, 720));
  CHECK(moov_size_of(roll2) == std::make_pair(1920, 1080));
  CHECK(samples_in(roll2) == 2);         // the 1080 IDR and its P
}

TEST(changed_parameter_sets_at_the_same_size_do_not_roll) {
  const std::string path = scratch("rawdvr_same_size.mp4");
  std::remove(path.c_str());
  RawDvr d;
  int calls = 0;
  d.set_next_sink([&](std::string* name) {
    ++calls;
    *name = scratch("rawdvr_same_size_next.mp4");
    return mabur::open_file_sink(*name);
  });
  d.start(path, 0, 0);
  feed(d, idr_au_720(1), 0);
  feed(d, idr_au_720_other_pps(2), 16667);  // new PPS bytes, same 1280x720
  CHECK(d.rolls() == 0);
  CHECK(calls == 0);
  CHECK(d.state() != RawDvr::State::Error);
  d.stop();
}

TEST(roll_without_a_next_sink_is_an_open_error) {
  const std::string path = scratch("rawdvr_noroll.mp4");
  std::remove(path.c_str());
  RawDvr d;  // no set_next_sink
  d.start(path, 0, 0);
  feed(d, idr_au_720(1), 0);
  feed(d, idr_au_1080(2), 16667);        // size change, nowhere to roll to
  CHECK(d.state() == RawDvr::State::Error);
  CHECK(d.err() == RawDvr::Err::Open);
  d.stop();
}

// The roll baseline is the SPS's own coded size, never a caller's
// width/height override: maburplay's configured screen mode (bcfg.width/
// height) can legitimately differ from the drone's actual encoder output
// (the documented fps/screen-mode mismatch), and that mismatch must not
// look like a resolution change on every single IDR.
TEST(roll_baseline_is_the_sps_not_the_override) {
  const std::string path = scratch("rawdvr_override.mp4");
  const std::string next_path = scratch("rawdvr_override_next.mp4");
  std::remove(path.c_str());
  std::remove(next_path.c_str());
  RawDvr d;
  int calls = 0;
  d.set_next_sink([&](std::string* name) {
    ++calls;
    *name = next_path;
    return mabur::open_file_sink(*name);
  });
  // Override 1920x1080 while the stream is actually 1280x720: the moov
  // records the override, but the roll baseline tracks the real SPS.
  d.start(path, 1920, 1080);
  feed(d, idr_au_720(1), 0);
  feed(d, p_au(2), 16667);
  feed(d, idr_au_720(3), 33333);   // same 720p SPS again: must not roll
  feed(d, p_au(4), 50000);
  CHECK(d.rolls() == 0);
  CHECK(calls == 0);
  CHECK(moov_size_of(path) == std::make_pair(1920, 1080));   // the override, unchanged

  feed(d, idr_au_1080(5), 66666);  // the SPS itself changes now: rolls
  feed(d, p_au(6), 83333);
  CHECK(d.rolls() == 1);
  CHECK(calls == 1);
  d.stop();
  // After the roll the stream is the authority: the new file's declared
  // size is the SPS size that forced it, not the first file's override
  // (same numeric value here, 1920x1080, but for a different reason).
  CHECK(moov_size_of(next_path) == std::make_pair(1920, 1080));
}

// Mirrors the web GS: next_sink can't produce a sink synchronously from
// inside feed() (OpfsFile::open() awaits through ASYNCIFY, which must run
// from the core loop, never from an AU callback -- opfs_file.h) -- it
// writes the rolled name and returns nullptr, and the caller finishes the
// roll later with start(sink, ...).
TEST(a_roll_can_defer_to_the_core_loop) {
  const std::string path = scratch("rawdvr_defer.mp4");
  const std::string next_path = scratch("rawdvr_defer_next.mp4");
  std::remove(path.c_str());
  std::remove(next_path.c_str());
  RawDvr d;
  int calls = 0;
  d.set_next_sink([&](std::string* name) {
    ++calls;
    *name = next_path;
    return std::unique_ptr<mabur::DvrSink>();  // defer: no sink yet
  });
  d.start(path, 0, 0);
  feed(d, idr_au_720(1), 0);
  feed(d, p_au(2), 16667);
  feed(d, idr_au_1080(3), 33333);   // size changes: defers rather than opening inline
  CHECK(calls == 1);
  CHECK(d.state() == RawDvr::State::WaitSync);
  CHECK(d.roll_deferred());
  CHECK(d.path() == next_path);
  CHECK(exists(path));                                      // the old file is sealed...
  CHECK(moov_size_of(path) == std::make_pair(1280, 720));    // ...and playable
  CHECK(!exists(next_path));                                 // nothing opened for it yet
  const uint64_t rolls_at_defer = d.rolls();
  const uint64_t sealed_at_defer = d.last_sealed_bytes();
  CHECK(rolls_at_defer == 1);
  CHECK(sealed_at_defer == read_whole_file(path).size());

  // Nothing was written for the triggering IDR -- it is lost to the gap
  // between the old file's last sample and the new file's first one, by
  // design (roll()'s comment). A non-key AU is still just ignored
  // (ordinary WaitSync semantics).
  feed(d, p_au(4), 50000);
  CHECK(d.state() == RawDvr::State::WaitSync);
  // A further IRAP arriving before the caller resumes must not reopen
  // anything with the not-yet-real sink_/path_ the defer left behind.
  feed(d, idr_au_1080(5), 66666);
  CHECK(d.state() == RawDvr::State::WaitSync);
  CHECK(d.roll_deferred());
  CHECK(calls == 1);          // next_sink was not asked again

  // The caller (the web GS, off the core loop by now) has a real sink.
  d.start(mabur::open_file_sink(next_path), 0, 0);
  CHECK(!d.roll_deferred());
  CHECK(d.rolls() == rolls_at_defer);               // old counters intact: not reset...
  CHECK(d.last_sealed_bytes() == sealed_at_defer);  // ...by the resuming start()

  feed(d, idr_au_1080(6), 83333);   // the deferred file's first sample
  feed(d, p_au(7), 100000);
  CHECK(d.state() == RawDvr::State::Recording);
  CHECK(d.rolls() == 1);
  d.stop();
  CHECK(exists(next_path));
  CHECK(moov_size_of(next_path) == std::make_pair(1920, 1080));
  CHECK(samples_in(next_path) == 2);   // the 1080p IDR and its P -- not the lost, earlier one
}

// stop() while a roll is deferred (the operator stopped the recording
// before the caller got around to resuming it off the core loop) must not
// leave roll_deferred() set: left set, the NEXT start(sink, ...) -- for an
// unrelated, later recording -- would wrongly think it was resuming this
// stopped roll and keep its rolls()/last_sealed_bytes() instead of
// starting with fresh counters.
TEST(stop_during_a_deferred_roll_clears_it_for_the_next_recording) {
  const std::string path = scratch("rawdvr_defer_stop.mp4");
  const std::string deferred_next = scratch("rawdvr_defer_stop_next.mp4");
  const std::string fresh_next = scratch("rawdvr_defer_stop_fresh.mp4");
  std::remove(path.c_str());
  std::remove(deferred_next.c_str());
  std::remove(fresh_next.c_str());
  RawDvr d;
  d.set_next_sink([&](std::string* name) {
    *name = deferred_next;
    return std::unique_ptr<mabur::DvrSink>();  // defer every roll in this test
  });
  d.start(path, 0, 0);
  feed(d, idr_au_720(1), 0);
  feed(d, idr_au_1080(2), 16667);   // defers: old file sealed, waiting on a sink
  CHECK(d.roll_deferred());
  CHECK(d.state() == RawDvr::State::WaitSync);

  d.stop();
  CHECK(!d.roll_deferred());
  CHECK(d.state() == RawDvr::State::Off);
  CHECK(!exists(deferred_next));   // never opened -- nothing to seal or report

  // A later, unrelated recording: fresh counters, not the stopped roll's.
  d.start(mabur::open_file_sink(fresh_next), 0, 0);
  CHECK(d.rolls() == 0);
  CHECK(d.last_sealed_bytes() == 0);
  feed(d, idr_au_720(3), 33334);
  CHECK(d.state() == RawDvr::State::Recording);
  CHECK(d.rolls() == 0);
  d.stop();
  CHECK(exists(fresh_next));
  CHECK(samples_in(fresh_next) == 1);
}

MTEST_MAIN
