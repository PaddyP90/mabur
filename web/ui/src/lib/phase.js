// Page-side pure logic for phase-locking the drone's camera to the
// browser's refresh (promoted from the wasm-spike throwaway). A continuous
// rAF loop feeds VsyncGrid the browser's own tick times; LatenessWindow
// tracks the clean-link delivery floor and the spread the regulator must
// cover, the same two numbers maburplay's FrameRegulator keeps; slot()
// turns a frame's pts into a release tick on the fitted grid; HoldQueue
// defers a decoded frame to its release tick instead of drawing it the
// instant WebCodecs hands it back, the way the regulator holds a frame for
// maburplay. No DOM or module calls here -- the video pipeline wires these
// in separately.

// Nearest-rank percentile with linear index rounding (not truncation): the
// fit below needs the true nearest sample, not a biased-low floor.
function pct(a, p) {
  if (!a.length) return NaN;
  const s = [...a].sort((x, y) => x - y);
  return s[Math.min(s.length - 1, Math.max(0, Math.round(p * (s.length - 1))))];
}

// Fits a period + phase to the browser's rAF timestamps, snapping a wobbly
// rAF callback time (observed +-1.9 ms while drawing, on macOS Chrome) back
// onto the steady grid underneath it. Refit every 30 ticks over the last
// 240: a least-squares period across steps near the median (so a missed
// frame's double-length step still counts, weighted by its multiple) and a
// median-residual phase (robust to the occasional wobbled sample).
export class VsyncGrid {
  constructor() {
    this._hist = [];
    this._n = 0;
    this._period = 0;
    this._ref = null;
    this._jitter = 0;
  }

  add(ts) {
    this._hist.push(ts);
    if (this._hist.length > 240) this._hist.shift();
    this._n++;
    if (this._n >= 30 && this._n % 30 === 0) this._fit();
  }

  get period() { return this._period; }
  get jitterMs() { return this._jitter; }

  // Before the first fit there is no grid yet: pass the timestamp through
  // unchanged rather than snapping to a period that doesn't exist.
  snap(ts) {
    if (!this._period || this._ref == null) return ts;
    return this._ref + Math.round((ts - this._ref) / this._period) * this._period;
  }

  nextTick(t) {
    if (!this._period || this._ref == null) return t;
    const k = Math.ceil((t - this._ref) / this._period);
    return this._ref + k * this._period;
  }

  _fit() {
    const r = this._hist.slice(-240);
    const d = [];
    for (let i = 1; i < r.length; i++) d.push(r[i] - r[i - 1]);
    if (!d.length) return;
    const m = pct(d, 0.5);
    let n = 0, t = 0;
    for (const x of d) {
      const k = Math.round(x / m);
      if (k >= 1 && k <= 3 && Math.abs(x - k * m) < 0.3 * m) { n += k; t += x; }
    }
    this._period = n ? t / n : m;
    const P = this._period;
    const ref = r[r.length - 1];
    const res = r.map((x) => { const y = x - ref; return y - Math.round(y / P) * P; });
    this._ref = ref + pct(res, 0.5);
    const stepRes = d.map((x) => x - Math.round(x / P) * P);
    this._jitter = pct(stepRes, 0.9) - pct(stepRes, 0.1);
  }
}

// Rolling p1 / p99 of frame transit (decode output - pts, ms): the page
// counterpart of maburplay's LatenessWindow (gs/player/src/lateness_window.h).
// p1 is the delivery floor, p99 - p1 + 1 the allowance a capture-time
// scheduler must add on top to absorb the clean-link spread. Both stay null
// until the window has enough samples to mean anything.
export class LatenessWindow {
  static kSize = 600;
  static kMinSamples = 60;
  static kAllowCapMs = 16;

  constructor() {
    this._buf = [];
  }

  add(dMs) {
    this._buf.push(dMs);
    if (this._buf.length > LatenessWindow.kSize) this._buf.shift();
  }

  get floor() {
    const k = this._k();
    return k == null ? null : this._sorted()[k];
  }

  get allow() {
    const k = this._k();
    if (k == null) return null;
    const s = this._sorted();
    return Math.min(s[s.length - 1 - k] - s[k] + 1, LatenessWindow.kAllowCapMs);
  }

  reset() { this._buf = []; }

  _k() {
    const n = this._buf.length;
    return n < LatenessWindow.kMinSamples ? null : Math.floor(n / 100);
  }

  _sorted() { return [...this._buf].sort((a, b) => a - b); }
}

// A frame's pts (ms) wants to show no earlier than floor + allow after
// capture; slot() resolves that want onto the fitted grid's next tick, the
// margin feeding the C++ PhaseLock exactly as the spike's onFrameInner did.
// null whenever either input is missing: the lateness window hasn't warmed
// up, or the grid has no period yet.
export function slot(ptsMs, floor, allow, grid) {
  if (floor == null || allow == null || !grid || !grid.period) return null;
  const want = ptsMs + floor + allow;
  const release = grid.nextTick(want);
  return { want, release, margin: release - want };
}

// Defers decoded frames to their release tick instead of drawing them the
// instant WebCodecs hands them back. Entries arrive in pts order (the
// caller's push order), so within a drain's due set the last one pushed is
// the newest and is the one actually drawn; the rest are stale duplicates
// of an already-superseded tick and are reported as displaced rather than
// drawn. lastDrained never moves backwards, so a straggler whose release
// tick already fired is refused outright instead of queuing forever.
export class HoldQueue {
  constructor() {
    this._q = [];
    this._lastDrained = -Infinity;
  }

  push(e) {
    if (e.release <= this._lastDrained) return false;
    this._q.push(e);
    return true;
  }

  drain(tick, period) {
    const cutoff = tick + period / 2;
    const due = [];
    const rest = [];
    for (const e of this._q) (e.release <= cutoff ? due : rest).push(e);
    this._q = rest;
    if (!due.length) return { draw: null, displaced: [] };
    const draw = due.pop();
    this._lastDrained = draw.release;
    return { draw, displaced: due };
  }

  flush() {
    const all = this._q;
    this._q = [];
    return all;
  }

  get length() { return this._q.length; }
}
