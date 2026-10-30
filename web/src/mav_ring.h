#pragma once
// MavRing: a FIFO of whole MAVLink packets bounded by total bytes. Not
// thread-safe on its own -- callers that share one across threads (the
// page/core bridge in web_main.cpp) hold their own mutex around every
// call, same as WebGs's own single-threaded API.
//
// Two owners:
//  - WebGs's inbound buffer (web_gs.h): drained every tick() in
//    production, so its cap is just a backstop.
//  - web_main.cpp's page bridge: the page may never call webgs_mav_take
//    (no UI consumes MAVLink yet), and the drone's once-a-second
//    heartbeat would otherwise grow it for the module's whole lifetime.
//    push() makes room for a new packet by dropping the OLDEST queued
//    packets first; a packet bigger than the whole cap is dropped on its
//    own. Both counted in dropped().
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace webgs {

constexpr size_t kMavRingDefaultCap = 64 * 1024;

class MavRing {
 public:
  explicit MavRing(size_t cap_bytes = kMavRingDefaultCap) : cap_(cap_bytes) {}

  // Queues one whole packet, dropping the oldest queued packets to make
  // room if it would push the ring past its cap; a packet larger than the
  // cap on its own is dropped instead of ever being stored partially.
  void push(const uint8_t* p, size_t n) {
    if (n > cap_) {
      ++dropped_;
      return;
    }
    while (!lens_.empty() && bytes_.size() + n > cap_) {
      bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<ptrdiff_t>(lens_.front()));
      lens_.pop_front();
      ++dropped_;
    }
    bytes_.insert(bytes_.end(), p, p + n);
    lens_.push_back(n);
  }

  // Pops the front packet into `out` if it fits in `cap` bytes -- one
  // packet per call, never a split one. Returns its length, or 0 if the
  // ring is empty or the front packet doesn't fit (left queued for the
  // next call, e.g. with a bigger buffer).
  size_t take(uint8_t* out, size_t cap) {
    if (lens_.empty() || lens_.front() > cap) return 0;
    const size_t n = lens_.front();
    std::memcpy(out, bytes_.data(), n);
    bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<ptrdiff_t>(n));
    lens_.pop_front();
    return n;
  }

  void clear() {
    bytes_.clear();
    lens_.clear();
    dropped_ = 0;
  }

  bool empty() const { return lens_.empty(); }
  size_t bytes() const { return bytes_.size(); }
  uint64_t dropped() const { return dropped_; }

 private:
  size_t cap_;
  std::vector<uint8_t> bytes_;
  std::deque<size_t> lens_;
  uint64_t dropped_ = 0;
};

}  // namespace webgs
