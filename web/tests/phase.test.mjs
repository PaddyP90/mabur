// node --test web/tests/phase.test.mjs (promoted from the wasm-spike throwaway)
import test from 'node:test';
import assert from 'node:assert/strict';
import { VsyncGrid, LatenessWindow, slot, HoldQueue } from '../ui/src/lib/phase.js';

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
