// Page side of phase-locking the drone's camera to the browser's refresh.
// A continuous rAF loop feeds VsyncGrid the browser's own tick times;
// LatenessWindow tracks the clean-link delivery floor and the spread the
// regulator must cover, the same two numbers maburplay's FrameRegulator
// keeps; slot() turns a frame's pts into a release tick on the fitted grid;
// HoldQueue defers a decoded frame to its release tick instead of drawing it
// the instant WebCodecs hands it back, the way the regulator holds a frame
// for maburplay. Those four are pure (no DOM, no module calls);
// PhaseController at the bottom is the glue to the wasm exports.

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
// 240: the period is a ratio estimate, sum(step) / sum(k) over the steps
// that land within 0.3 of the median step of a whole multiple k (1-3) of
// it -- so a missed frame's double-length step still counts, as two
// periods -- and the phase is the median residual (robust to the
// occasional wobbled sample).
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
// capture; slot() resolves that want onto the fitted grid's next tick. The
// margin (release - want) is what the C++ PhaseLock steers toward its
// target: how far ahead of its release tick the frame was ready.
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

const PHASE_STATE_NAMES = { 0: 'wait', 1: 'acq', 2: 'lock', 3: 'oor' };

// The drone's pts is 32-bit us; frame.timestamp is the page's unwrapped
// timeline, which passes 2^32 after ~71.6 min of drone uptime. The core's
// source-rate estimate wants the drone's own value back.
const TWO32 = 4294967296;
const low32 = (us) => ((us % TWO32) + TWO32) % TWO32;

// Glue between the page classes above and the nine wasm exports: feeds the
// grid and the lateness window from the decoder's output and the page's
// own rAF, drives the C++ PhaseLock through its tick/frame/discontinuity
// calls, and decides whether a decoded frame should wait for its release
// tick (held in a HoldQueue) or draw right away. getModule() is null until
// the wasm core attaches (or forever, in spotter mode, where the caller
// never touches this class at all). Every export goes through _mod(): the
// first time a module it has not reset shows up, it is reset before any
// other call reaches it -- reset() itself usually runs before the session
// has a module at all. onEvicted(entries) is how a flush that
// happens with no caller waiting for a return value (a disable, a transit
// jump, a hidden tab caught by the tick timer) still reaches the pipeline:
// it owns every VideoFrame.close() and the bookkeeping that goes with it.
export class PhaseController {
  constructor({ getModule, getLowPower = () => null, onEvicted = () => {} }) {
    this.getModule = getModule;
    this.getLowPower = getLowPower;
    this.onEvicted = onEvicted;
    this._enabled = true;
    this._timer = null;
    // Build the per-session state directly rather than calling reset():
    // reset() calls getModule() to re-arm a live module, but at
    // construction time the caller's accessor may not be safe to call yet
    // (e.g. it closes over a `session` the caller hasn't declared). The
    // module only ever needs resetting on first use, and _mod() already
    // handles that.
    this._resetMod = null;
    this.grid = new VsyncGrid();
    this.window = new LatenessWindow();
    this.queue = new HoldQueue();
    this._lastRafTick = -Infinity;
    this._offset = 0;
    this._held = 0;
    this._late = 0;
    this._displacedTotal = 0;
  }

  get enabled() { return this._enabled; }
  // Off: nothing queued may outlive the moment it's disabled -- evict now,
  // not whenever the queue would otherwise have next drained.
  set enabled(on) {
    on = !!on;
    if (!on && this._enabled) this._evict();
    this._enabled = on;
  }

  _evict() {
    const entries = this.queue.flush();
    if (entries.length) this.onEvicted(entries);
    return entries;
  }

  // New session: re-arms the C++ lock and starts the grid/window/queue
  // over. Doesn't touch `enabled` -- that's a standing preference, not
  // per-session state. With no module yet, the next one _mod() sees is
  // reset on first use instead. Returns whatever was still queued
  // (normally nothing -- the caller flushes first -- kept as a ledger
  // rather than silently dropped).
  reset() {
    const mod = this.getModule();
    this._resetMod = mod || null;
    if (mod) mod._webgs_phase_reset();
    const dropped = this.queue ? this.queue.flush() : [];
    this.grid = new VsyncGrid();
    this.window = new LatenessWindow();
    this.queue = new HoldQueue();
    this._lastRafTick = -Infinity;
    this._offset = 0;
    this._held = 0;
    this._late = 0;
    this._displacedTotal = 0;
    return dropped;
  }

  start() {
    this.stop();
    const h = setInterval(() => {
      this.tick(performance.now(), { lowPower: this.getLowPower(), visible: !document.hidden });
    }, 50);
    // A browser setInterval handle has no .unref; Node's test runner needs
    // it so a PhaseController nobody explicitly stop()ped doesn't wedge
    // the process open.
    if (h && typeof h.unref === 'function') h.unref();
    this._timer = h;
  }

  stop() {
    if (this._timer != null) { clearInterval(this._timer); this._timer = null; }
  }

  // The live module, re-armed exactly once the first time it differs from
  // the one last reset (a Connect's module appears after reset() ran).
  _mod() {
    const mod = this.getModule();
    if (mod && mod !== this._resetMod) {
      this._resetMod = mod;
      mod._webgs_phase_reset();
    }
    return mod;
  }

  _stateName() {
    const mod = this._mod();
    if (!mod) return 'off';
    return PHASE_STATE_NAMES[mod._webgs_phase_state()] ?? 'off';
  }

  // Feeds the grid from the page's continuous rAF loop. Returns the newest
  // frame due to present at this tick (if any) plus any it displaced; the
  // caller (VideoPipeline) draws/closes the one and closes the rest.
  onRaf(ts) {
    this.grid.add(ts);
    const tick = this.grid.snap(ts);
    this._lastRafTick = tick;
    if (!this.queue.length) return { draw: null, displaced: [] };
    const { draw, displaced } = this.queue.drain(tick, this.grid.period || 0);
    if (displaced.length) this._displacedTotal += displaced.length;
    return { draw: draw ? { frame: draw.frame, tick } : null, displaced };
  }

  // A decoded frame: feeds the lateness window and the C++ lock, and
  // decides whether to queue it for its release tick ('hold') or hand it
  // back for an immediate draw ('now').
  onFrame({ frame, ptsUs, tOut }) {
    const ptsMs = ptsUs / 1000;
    const d = tOut - ptsMs;
    const floorBefore = this.window.floor;
    let discontinuity = false;
    if (floorBefore != null && Math.abs(d - floorBefore) > 1000) {
      discontinuity = true;
      const mod = this._mod();
      if (mod) mod._webgs_phase_discontinuity(tOut * 1000);
      this.window.reset();
      // These were queued before the jump; onFrame only returns a verdict
      // for the frame in hand, so hand the rest to onEvicted instead of
      // leaking them.
      this._evict();
    }
    this.window.add(d);

    const s = slot(ptsMs, this.window.floor, this.window.allow, this.grid);
    const mod = this._mod();
    // pts always feeds the lock's source-rate estimate; the margin is NaN
    // whenever slot() couldn't resolve one (window/grid not warm yet).
    if (mod) mod._webgs_phase_frame(low32(ptsUs), s ? s.margin * 1000 : NaN, this.grid.period * 1000, tOut * 1000);

    if (discontinuity) return 'now';
    if (!this._enabled || document.hidden || this._stateName() !== 'lock') return 'now';
    if (mod && mod._webgs_phase_mismatch()) return 'now';
    if (!s) return 'now';
    // Late: this release tick's rAF already ran.
    if (s.release <= this._lastRafTick) { this._late++; return 'now'; }
    // More than 3 periods ahead is a bad estimate, not a late one.
    const period = this.grid.period;
    if (period && (s.release - tOut) > 3 * period) return 'now';
    if (!this.queue.push({ frame, release: s.release, tOut })) { this._late++; return 'now'; }
    this._held++;
    return 'hold';
  }

  // The 50 ms driver: feeds the C++ lock's tick and tracks its last
  // offset -- the real one, always, since the core already put its return
  // straight into the RCF by the time this call returns; hiding it behind
  // `enabled` here would only mislead snapshot(), not change what the
  // drone received. Disabled reports not-visible instead, so the core lets
  // go of the offset immediately rather than riding out its own staleness
  // timeout. A hidden tab will also never present a held frame, so its
  // queue is evicted here as a backup for whenever this call lands --
  // _startPhaseLoop's own visibilitychange listener is the fast path and
  // usually gets there first.
  tick(nowMs, { lowPower, visible }) {
    const mod = this._mod();
    const panelHz = this.grid.period ? 1000 / this.grid.period : 0;
    const lp = lowPower === true ? 1 : lowPower === false ? 0 : 2;
    const vis = this._enabled && visible ? 1 : 0;
    this._offset = mod ? mod._webgs_phase_tick(nowMs * 1000, panelHz, lp, vis) : 0;
    if (!visible) this._evict();
    return this._offset;
  }

  // Queued entries for an explicit caller to close: a session reset or
  // close, or a hidden tab. Internal evictions (disable, discontinuity,
  // hidden tab caught by the tick) go through onEvicted instead -- nothing
  // calls this to receive those.
  flush() { return this.queue.flush(); }

  // The same, for a 'now' verdict that must not let an older held frame
  // outlive the frame it just drew: those never reach the screen, so they
  // count as displaced.
  displaceQueued() {
    const entries = this.queue.flush();
    this._displacedTotal += entries.length;
    return entries;
  }

  snapshot() {
    const mod = this._mod();
    return {
      state: this._stateName(),
      errMs: mod ? mod._webgs_phase_err_ms() : NaN,
      offset: this._offset,
      centre: mod ? mod._webgs_phase_centre() : NaN,
      panelHz: this.grid.period ? 1000 / this.grid.period : null,
      srcHz: mod ? mod._webgs_phase_src_hz() : NaN,
      mismatch: mod ? !!mod._webgs_phase_mismatch() : false,
      held: this._held,
      late: this._late,
      displaced: this._displacedTotal,
      enabled: this._enabled,
    };
  }
}
