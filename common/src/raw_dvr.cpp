#include "mabur/raw_dvr.h"

namespace mabur {

bool au_has_param_sets(const uint8_t* au, size_t n) {
  bool v = false, s = false, p = false;
  for (const NalView& nal : split_nals(au, n)) {
    v |= nal.type == 32;
    s |= nal.type == 33;
    p |= nal.type == 34;
  }
  return v && s && p;
}

void RawDvr::start(const std::string& path, int width, int height, int fragment_ms) {
  stop();
  path_ = path;
  width_ = width;
  height_ = height;
  fragment_ms_ = fragment_ms;
  opened_ = false;
  roll_deferred_ = false;
  sps_open_w_ = 0;
  sps_open_h_ = 0;
  err_ = Err::None;
  state_ = State::WaitSync;
  rolls_ = 0;
  last_sealed_bytes_ = 0;
}

void RawDvr::set_next_sink(NextSink fn) { next_sink_ = std::move(fn); }

bool RawDvr::roll(int w, int h) {
  // close() flushes whatever fragment was still pending -- bytes_written()
  // must be read AFTER it, or the sealed size undercounts by that
  // fragment.
  mux_.close();
  last_sealed_bytes_ = mux_.bytes_written();
  opened_ = false;
  if (!next_sink_) {
    state_ = State::Error;
    err_ = Err::Open;
    return false;
  }
  std::string name;
  std::unique_ptr<DvrSink> sink = next_sink_(&name);
  if (!sink) {
    if (name.empty()) {  // no sink and no name either: nowhere to go
      state_ = State::Error;
      err_ = Err::Open;
      return false;
    }
    // Deferred: the callback can't produce a sink synchronously from in
    // here (the web GS's OPFS open awaits through ASYNCIFY, which must run
    // from the core loop, never from inside this AU callback --
    // opfs_file.h). The caller resumes with start(sink, ...) once it has
    // one; the triggering IRAP is not written anywhere -- the next sync
    // point in the new file writes it.
    path_ = name;
    roll_deferred_ = true;
    state_ = State::WaitSync;
    ++rolls_;
    return false;
  }
  if (!mux_.open(std::move(sink), params_.hvcc(), w, h, fragment_ms_) || !mux_.ok()) {
    mux_.close();
    state_ = State::Error;
    err_ = Err::Open;
    return false;
  }
  path_ = name;
  // After a roll the stream is the authority for the roll baseline: the
  // width/height override (if any) only ever shaped the FIRST file.
  sps_open_w_ = w;
  sps_open_h_ = h;
  opened_ = true;
  ++rolls_;
  return true;
}

void RawDvr::start(std::unique_ptr<DvrSink> sink, int width, int height, int fragment_ms) {
  // Resuming a deferred roll (see roll()): the counters it already moved
  // -- rolls_ counted the roll when it deferred, last_sealed_bytes_ holds
  // the file roll() sealed -- describe this recording, not a fresh one, so
  // they must survive the reset below.
  const bool resuming_roll = roll_deferred_;
  const uint64_t saved_rolls = rolls_;
  const uint64_t saved_last_sealed_bytes = last_sealed_bytes_;
  start(std::string(), width, height, fragment_ms);
  if (resuming_roll) {
    rolls_ = saved_rolls;
    last_sealed_bytes_ = saved_last_sealed_bytes;
  }
  if (!sink) {
    state_ = State::Error;
    err_ = Err::Open;
    return;
  }
  sink_ = std::move(sink);
}

void RawDvr::feed(const uint8_t* au, size_t n, uint32_t pts_us, bool complete) {
  if (state_ != State::WaitSync && state_ != State::Recording) return;
  if (!complete) return;  // DVR records complete AUs only; DvrMux cuts at the next key
  if (au_has_param_sets(au, n)) params_.feed(au, n);  // sticky across recordings, never reset
  // Sync = a real IRAP. The live encoder is GDR: VPS/SPS/PPS also ride on
  // every TRAIL_R refresh start, which references frames before it.
  // Treating those as sync opened files on a P slice and flagged P slices
  // as random-access points -- Apple's decoder (QuickTime, VLC on macOS)
  // refused the whole file (seen on a web-GS recording).
  const bool key = au_is_irap(au, n);
  if (state_ == State::WaitSync) {
    // A file must BEGIN at a sync point. params_ is sticky, so on a second
    // recording complete() is already true -- the `key` clause is what keeps
    // the file from opening on an AU whose references are not in it.
    if (!key || !params_.complete()) return;
    // A deferred roll (see roll()) is waiting for start(sink, ...): sink_
    // and path_ are not a real file to open yet, just the rolled file's
    // future name, so there is nothing to do here until that call clears
    // the flag.
    if (roll_deferred_) return;
    int w = width_, h = height_;
    if ((w <= 0 || h <= 0) && !params_.sps_dimensions(&w, &h)) {
      w = 1920;
      h = 1080;
    }
    const bool ok = sink_ ? mux_.open(std::move(sink_), params_.hvcc(), w, h, fragment_ms_)
                          : mux_.open(path_, params_.hvcc(), w, h, fragment_ms_);
    if (!ok || !mux_.ok()) {
      mux_.close();
      state_ = State::Error;
      err_ = Err::Open;
      return;
    }
    opened_ = true;
    // Roll baseline: the SPS's OWN size, never the width/height override --
    // a configured screen mode that mismatches the drone's actual encoder
    // output (the documented fps/screen-mode mismatch case) must not look
    // like a resolution change. 0x0 when it doesn't parse; see sps_open_w_.
    if (!params_.sps_dimensions(&sps_open_w_, &sps_open_h_)) {
      sps_open_w_ = 0;
      sps_open_h_ = 0;
    }
    state_ = State::Recording;
  } else if (opened_ && key) {
    // The file's header carries one size; a coded-size change (the drone
    // picked a new camera mode mid-session) can only be served by sealing
    // this file and starting the next one, cut right at the IRAP that
    // changed the SPS. Changed VPS/SPS/PPS bytes at the SAME size are not a
    // roll -- the live GDR encoder rewrites them on every refresh start.
    int w = 0, h = 0;
    if (params_.sps_dimensions(&w, &h)) {
      if (sps_open_w_ == 0 && sps_open_h_ == 0) {
        // The SPS didn't parse at open (or the last roll): nothing to
        // compare against yet, so this IRAP's SPS becomes the baseline
        // without rolling.
        sps_open_w_ = w;
        sps_open_h_ = h;
      } else if (w != sps_open_w_ || h != sps_open_h_) {
        if (!roll(w, h)) return;
      }
    }
  }
  mux_.write_sample(au, n, pts_us, key);
  if (!mux_.ok()) {  // card/quota full: keep what was written, stop feeding
    mux_.close();
    state_ = State::Error;
    err_ = Err::Write;
  }
}

void RawDvr::stop() {
  if (state_ == State::Recording) mux_.close();
  sink_.reset();  // an unopened sink: dropped unwritten
  if (state_ != State::Off) state_ = State::Off;
}

}  // namespace mabur
