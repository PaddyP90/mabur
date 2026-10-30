#ifndef MABUR_RAW_DVR_H_
#define MABUR_RAW_DVR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "mabur/dvr_mux.h"
#include "mabur/hevc_params.h"

namespace mabur {

// True when the AU carries VPS, SPS and PPS: the live GDR encoder's refresh
// start AND every IDR. Feeds the hvcC; NOT a sync point on its own -- a
// refresh start is a P slice. Recordings begin, and fragments are marked
// sync, only at au_is_irap() AUs.
bool au_has_param_sets(const uint8_t* au, size_t n);

// The raw DVR's recording rules, shared by maburplay's raw mode and the web
// GS: arm, wait for a sync point, write complete AUs through DvrMux, seal.
// Single-threaded.
class RawDvr {
 public:
  enum class State { Off, WaitSync, Recording, Error };
  enum class Err { None, Open, Write };

  // Arms a recording to `path` (sealing any open one first). The file is
  // created on the first IRAP once VPS/SPS/PPS are known; nothing before
  // that. IDRs are rare on the GDR link (seconds apart), so a caller that
  // can should ask the drone for one when it arms.
  // width/height <= 0: take them from the SPS (1920x1080 if it won't parse).
  void start(const std::string& path, int width, int height, int fragment_ms = 1000);

  // Same, into an already-open sink (the web GS's OPFS file), held until the
  // first sync point; dropped unwritten if stop() comes first. null = Err::Open.
  void start(std::unique_ptr<DvrSink> sink, int width, int height, int fragment_ms = 1000);

  // Opens the next file when the coded size changes mid-recording (see
  // feed()). Called with the sink about to become current; writes that
  // file's display name into `*name` for path() to report afterwards. A
  // null fn, or one that returns a null sink, is an open error.
  using NextSink = std::function<std::unique_ptr<DvrSink>(std::string* name)>;
  void set_next_sink(NextSink fn);

  // Every AU, in arrival order. Truncated AUs are skipped whole. Ignored
  // unless armed or recording. Rolls to the next sink when an IRAP's SPS
  // reports a coded size different from the open file's -- one file's
  // header can describe only one size.
  void feed(const uint8_t* au, size_t n, uint32_t pts_us, bool complete);

  // Flushes and closes an open file; Off afterwards. err() is kept so the
  // caller can report why a recording ended.
  void stop();

  State state() const { return state_; }
  Err err() const { return err_; }
  const std::string& path() const { return path_; }
  bool opened() const { return opened_; }
  uint64_t bytes() const { return opened_ ? mux_.bytes_written() : 0; }
  uint64_t samples() const { return opened_ ? mux_.samples() : 0; }
  uint64_t fragments() const { return opened_ ? mux_.fragments() : 0; }
  // How many times feed() has rolled to a new file (successful rolls only).
  uint64_t rolls() const { return rolls_; }
  // Bytes the file sealed by the most recent roll held, for a caller (the
  // web GS) that needs to report the sealed file's size after the roll has
  // already moved bytes()/path() on to the new file.
  uint64_t last_sealed_bytes() const { return last_sealed_bytes_; }

 private:
  // Seals the open file and opens the next one on `next_sink_` for a new
  // (w, h). Returns false (state_ = Error, err_ = Open) on a missing
  // callback, a null sink, or a mux open failure.
  bool roll(int w, int h);

  HevcParams params_;
  DvrMux mux_;
  State state_ = State::Off;
  Err err_ = Err::None;
  std::string path_;
  std::unique_ptr<DvrSink> sink_;  // start(sink) until the file opens
  bool opened_ = false;
  int width_ = 0, height_ = 0, fragment_ms_ = 1000;
  int width_open_ = 0, height_open_ = 0;  // the size the open file was created with
  NextSink next_sink_;
  uint64_t rolls_ = 0;
  uint64_t last_sealed_bytes_ = 0;
};

}  // namespace mabur

#endif  // MABUR_RAW_DVR_H_
