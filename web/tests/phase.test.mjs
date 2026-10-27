// node --test web/tests/phase.test.mjs (promoted from the wasm-spike throwaway)
import test from 'node:test';
import assert from 'node:assert/strict';
import { VsyncGrid, LatenessWindow, slot, HoldQueue, PhaseController } from '../ui/src/lib/phase.js';

globalThis.document ??= { hidden: false };

const PERIOD = 16.6667;

test('grid fits a steady 60 Hz', () => {
  const g = new VsyncGrid();
  for (let k = 0; k < 300; k++) g.add(k * PERIOD);
  assert.ok(Math.abs(g.period - PERIOD) < 0.001, `period ${g.period}`);
  assert.ok(g.jitterMs < 0.01, `jitterMs ${g.jitterMs}`);
});

test('grid ignores a ±1.9 ms wobble', () => {
  const g = new VsyncGrid();
  let wobbleUp = true;
  const wobbled = [];  // [trueTick, observedTs] for every k % 3 === 1
  for (let k = 0; k < 300; k++) {
    const trueTick = k * PERIOD;
    let ts = trueTick;
    if (k % 3 === 1) {
      ts += wobbleUp ? 1.9 : -1.9;
      wobbleUp = !wobbleUp;
      wobbled.push([trueTick, ts]);
    }
    g.add(ts);
  }
  // A wobbled timestamp well inside the fitted window should snap back to
  // its true tick, not the raw ±1.9 ms reading.
  const [trueTick, wobbledTs] = wobbled[wobbled.length - 2];
  assert.ok(Math.abs(g.snap(wobbledTs) - trueTick) < 0.2,
    `snap ${g.snap(wobbledTs)} vs true ${trueTick}`);
});

test('grid skips missed frames', () => {
  const g = new VsyncGrid();
  let n = 0;
  for (let k = 0; k < 400; k++) {
    if ((k + 1) % 10 === 0) continue;  // drop every 10th tick
    g.add(k * PERIOD);
    n++;
  }
  assert.ok(n > 300, `only added ${n} ticks`);
  assert.ok(Math.abs(g.period - PERIOD) < 0.01, `period ${g.period}`);
});

test('grid has no period before the first fit', () => {
  const g = new VsyncGrid();
  for (let k = 0; k < 29; k++) g.add(k * PERIOD);
  assert.equal(g.period, 0);
  assert.equal(g.snap(123.45), 123.45);
  assert.equal(g.nextTick(123.45), 123.45);
});

test('lateness window floor and allow', () => {
  const w = new LatenessWindow();
  for (let i = 0; i < 600; i++) w.add(100 + (i % 10));
  assert.equal(w.floor, 100);
  assert.equal(w.allow, 10);  // p99 109 - p1 100 + 1
});

test('lateness window allow caps at 16', () => {
  const w = new LatenessWindow();
  for (let i = 0; i < 600; i++) w.add(i % 41);  // 0..40 spread
  assert.equal(w.floor, 0);
  assert.equal(w.allow, 16);
});

test('lateness window is null below 60 samples', () => {
  const w = new LatenessWindow();
  for (let i = 0; i < 59; i++) w.add(100);
  assert.equal(w.floor, null);
  assert.equal(w.allow, null);
});

test('lateness window resets on a transit jump', () => {
  const w = new LatenessWindow();
  for (let i = 0; i < 600; i++) w.add(100 + (i % 10));
  w.reset();
  assert.equal(w.floor, null);
  assert.equal(w.allow, null);
});

test('slot returns null without a floor/allow or a fitted grid', () => {
  const g = new VsyncGrid();
  for (let k = 0; k < 29; k++) g.add(k * PERIOD);  // not yet fitted
  assert.equal(slot(1000, null, 6, g), null);
  assert.equal(slot(1000, 5, null, g), null);
  assert.equal(slot(1000, 5, 6, g), null);
});

test('slot math', () => {
  const g = new VsyncGrid();
  for (let k = 0; k < 300; k++) g.add(k * PERIOD);  // grid anchored at 0
  const s = slot(1000, 5, 6, g);
  assert.equal(s.want, 1011);
  // First multiple of the period at or above want (1011): ceil(1011/16.6667) * 16.6667.
  assert.ok(Math.abs(s.release - 1016.6687) < 1e-3, `release ${s.release}`);
  assert.ok(Math.abs(s.margin - 5.6687) < 1e-3, `margin ${s.margin}`);
});

test('late entries are refused', () => {
  const q = new HoldQueue();
  q.push({ release: 100 });
  const { draw } = q.drain(100, PERIOD);
  assert.equal(draw.release, 100);
  assert.equal(q.push({ release: 100 }), false);
});

test('drain never goes backwards', () => {
  const q = new HoldQueue();
  q.push({ release: 100 });
  q.push({ release: 116.7 });
  const { draw, displaced } = q.drain(116.7, PERIOD);
  assert.equal(draw.release, 116.7);
  assert.equal(displaced.length, 1);
  assert.equal(displaced[0].release, 100);
});

test('drain reports nothing due yet', () => {
  const q = new HoldQueue();
  q.push({ release: 200 });
  const { draw, displaced } = q.drain(100, PERIOD);
  assert.equal(draw, null);
  assert.deepEqual(displaced, []);
  assert.equal(q.length, 1);
});

test('flush empties the queue and returns every entry', () => {
  const q = new HoldQueue();
  q.push({ release: 100 });
  q.push({ release: 116.7 });
  const all = q.flush();
  assert.equal(all.length, 2);
  assert.equal(q.length, 0);
});

// --- PhaseController: glue to the nine wasm exports + the hold decision ---

function fakeModule() {
  const calls = [];
  const mod = {
    state: -1, errMs: 0, centre: 0, mismatch: 0, srcHz: 0, offset: 0,
    _webgs_phase_reset() { calls.push(['reset']); },
    _webgs_phase_frame(ptsUs, marginUs, periodUs, nowUs) { calls.push(['frame', ptsUs, marginUs, periodUs, nowUs]); },
    _webgs_phase_discontinuity(nowUs) { calls.push(['discontinuity', nowUs]); },
    _webgs_phase_tick(nowUs, panelHz, lowPower, visible) {
      calls.push(['tick', nowUs, panelHz, lowPower, visible]);
      return mod.offset;
    },
    _webgs_phase_state() { return mod.state; },
    _webgs_phase_err_ms() { return mod.errMs; },
    _webgs_phase_centre() { return mod.centre; },
    _webgs_phase_mismatch() { return mod.mismatch; },
    _webgs_phase_src_hz() { return mod.srcHz; },
  };
  return { mod, calls };
}

// Warms the grid (ticks, period PERIOD) and the lateness window (120
// frames at a steady 20 ms transit, floor 20 / allow 1) together, on one
// shared timeline -- so a frame submitted right after warmup lands on the
// next grid tick instead of looking "late" against a stale lastRafTick.
function warmPhase(c, n = 120) {
  const frame = () => ({ close() {} });
  for (let i = 0; i < n; i++) {
    const tick = i * PERIOD;
    c.onRaf(tick);
    const tOut = tick;
    c.onFrame({ frame: frame(), ptsUs: (tOut - 20) * 1000, tOut });
  }
}

test('controller holds only when locked', () => {
  const { mod } = fakeModule();
  const c = new PhaseController({ getModule: () => mod, getLowPower: () => null });
  c.reset();
  warmPhase(c, 120);

  mod.state = 2;   // lock
  const tOut1 = 120 * PERIOD;
  const f1 = { close() {} };
  assert.equal(c.onFrame({ frame: f1, ptsUs: (tOut1 - 20) * 1000, tOut: tOut1 }), 'hold');

  mod.state = 1;   // acq
  const tOut2 = tOut1 + PERIOD;
  const f2 = { close() {} };
  assert.equal(c.onFrame({ frame: f2, ptsUs: (tOut2 - 20) * 1000, tOut: tOut2 }), 'now');
});

test('disable flushes and stops holding', () => {
  const { mod } = fakeModule();
  const c = new PhaseController({ getModule: () => mod, getLowPower: () => null });
  c.reset();
  warmPhase(c, 120);
  mod.state = 2;

  const mk = (i) => {
    const f = { closed: false, close() { this.closed = true; } };
    const tOut = (120 + i) * PERIOD;
    return { f, tOut, ptsUs: (tOut - 20) * 1000 };
  };
  const a = mk(0), b = mk(1);
  assert.equal(c.onFrame({ frame: a.f, ptsUs: a.ptsUs, tOut: a.tOut }), 'hold');
  assert.equal(c.onFrame({ frame: b.f, ptsUs: b.ptsUs, tOut: b.tOut }), 'hold');

  c.enabled = false;
  const flushed = c.flush();
  assert.equal(flushed.length, 2);

  const d = mk(2);
  assert.equal(c.onFrame({ frame: d.f, ptsUs: d.ptsUs, tOut: d.tOut }), 'now');
});

test('hidden tab flushes the hold queue', () => {
  const { mod, calls } = fakeModule();
  const c = new PhaseController({ getModule: () => mod, getLowPower: () => null });
  c.reset();
  warmPhase(c, 120);
  mod.state = 2;

  const mk = (i) => {
    const f = { closed: false, close() { this.closed = true; } };
    const tOut = (120 + i) * PERIOD;
    return { f, tOut, ptsUs: (tOut - 20) * 1000 };
  };
  const a = mk(0), b = mk(1);
  c.onFrame({ frame: a.f, ptsUs: a.ptsUs, tOut: a.tOut });
  c.onFrame({ frame: b.f, ptsUs: b.ptsUs, tOut: b.tOut });

  c.tick(b.tOut, { lowPower: null, visible: false });
  const tickCall = calls.filter((x) => x[0] === 'tick').pop();
  assert.equal(tickCall[4], 0);
  assert.equal(c.flush().length, 0);
  assert.equal(a.f.closed, true);
  assert.equal(b.f.closed, true);
});

test('transit jump resets', () => {
  const { mod, calls } = fakeModule();
  const c = new PhaseController({ getModule: () => mod, getLowPower: () => null });
  c.reset();
  for (let i = 0; i < 100; i++) {
    const tOut = i * PERIOD;
    c.onFrame({ frame: { close() {} }, ptsUs: (tOut - 20) * 1000, tOut });
  }
  const tOut = 100 * PERIOD;
  c.onFrame({ frame: { close() {} }, ptsUs: (tOut - 5020) * 1000, tOut });
  assert.ok(calls.some((x) => x[0] === 'discontinuity'), 'discontinuity fired');
});

test('controller maps drone_lp', () => {
  const { mod, calls } = fakeModule();
  const c = new PhaseController({ getModule: () => mod, getLowPower: () => null });
  c.reset();
  c.tick(0, { lowPower: true, visible: true });
  assert.equal(calls.filter((x) => x[0] === 'tick').pop()[3], 1);
  c.tick(0, { lowPower: false, visible: true });
  assert.equal(calls.filter((x) => x[0] === 'tick').pop()[3], 0);
  c.tick(0, { lowPower: null, visible: true });
  assert.equal(calls.filter((x) => x[0] === 'tick').pop()[3], 2);
});
