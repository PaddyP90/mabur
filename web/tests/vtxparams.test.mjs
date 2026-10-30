import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {
  MODE_SIZES, modeSizes, modeText, parseMode, commitText, SECTION_KICKER, groupRows,
  HINTS, applyBadge, headerText, rowView, showRefresh,
} from '../ui/src/lib/vtxparams.js';

test('modeSizes per sensor mode and at the boundaries', () => {
  assert.deepEqual(modeSizes(90), ['1920x1080', '1280x720']);
  assert.deepEqual(modeSizes(91), ['1728x972', '1280x720']);
  assert.deepEqual(modeSizes(100), ['1728x972', '1280x720']);
  assert.deepEqual(modeSizes(101), ['1728x816', '1152x544', '864x408']);
  assert.deepEqual(modeSizes(120), ['1728x816', '1152x544', '864x408']);
  assert.deepEqual(modeSizes(0), []);
  assert.deepEqual(modeSizes(121), []);
  assert.deepEqual(modeSizes(60.5), []);
  assert.deepEqual(modeSizes('90'), []);
  assert.equal(MODE_SIZES.length, 3);
});

test('modeText and parseMode round trip', () => {
  assert.equal(modeText(120, '1728x816'), '120@1728x816');
  assert.deepEqual(parseMode('60@1920x1080'), { fps: 60, size: '1920x1080' });
  assert.equal(parseMode('60@'), null);
  assert.equal(parseMode('x'), null);
  assert.equal(parseMode(''), null);
  assert.equal(parseMode('60@1920x1080extra'), null);
});

test('commitText per type', () => {
  assert.equal(commitText('bool', true), '1');
  assert.equal(commitText('bool', false), '0');
  assert.equal(commitText('bool', 'on'), '1');
  assert.equal(commitText('bool', 'off'), '0');
  assert.equal(commitText('bool', 'maybe'), null);
  assert.equal(commitText('int', ' 30000 '), '30000');
  assert.equal(commitText('int', '3e4'), null);
  assert.equal(commitText('int', '1.5'), null);
  assert.equal(commitText('real', '0.65'), '0.65');
  assert.equal(commitText('real', 'abc'), null);
  assert.equal(commitText('real', 'Infinity'), null);
  assert.equal(commitText('str', '1920x1080'), '1920x1080');
  assert.equal(commitText('str', ''), null);
});

test('commitText rejects strings the drone cannot hold', () => {
  assert.equal(commitText('str', 'a"b'), null);
  assert.equal(commitText('str', 'a\\b'), null);
  assert.equal(commitText('str', 'ñ'), null);
  assert.equal(commitText('str', 'a'.repeat(128)), null);
  assert.equal(commitText('str', 'a'.repeat(127)), 'a'.repeat(127));
});

const ROWS = [
  { id: 'RADIO_WALL_MARGN', section: 'radio' },
  { id: 'RADIO_LDPC', section: 'radio' },
  { id: 'VENC_MODE', section: 'venc' },
  { id: 'VENC_SLICES', section: 'venc' },
  { id: 'ENC_BITRATE_MAX', section: 'encoder' },
  { id: 'ENC_AIRTIME', section: 'encoder' },
  { id: 'REC_ENABLE', section: 'record' },
  { id: 'REC_BITRATE', section: 'record' },
  { id: 'REC_FPS', section: 'record' },
  { id: 'REC_SIZE', section: 'record' },
  { id: 'LOWPWR_ENABLE', section: 'low_power' },
  { id: 'MSP_ENABLE', section: 'msp' },
];

test('groupRows keeps table order under section kickers', () => {
  const groups = groupRows(ROWS);
  assert.deepEqual(groups.map((g) => g.kicker),
    ['Radio', 'Camera', 'Encoder', 'Recorder', 'Power', 'Flight controller']);
  const camera = groups.find((g) => g.key === 'venc');
  assert.deepEqual(camera.rows.map((r) => r.id), ['VENC_MODE', 'VENC_SLICES']);
});

test('headerText by state', () => {
  assert.equal(headerText({ linked: false, mode: 'spotter', rows: [] }, 'spotter'),
    'VTX settings need GS mode.');
  assert.equal(headerText({ linked: false }, 'gs'), 'Waiting for the drone link…');
  assert.equal(headerText({ linked: true, mismatch: true }, 'gs'),
    "The drone's parameter table differs from this page — redeploy.");
  assert.equal(headerText({ linked: true, mismatch: false, list: 'pending' }, 'gs'),
    "Reading the drone's settings…");
  assert.equal(headerText({ linked: true, mismatch: false, list: 'partial' }, 'gs'),
    'Some settings did not answer.');
  assert.equal(headerText({ linked: true, mismatch: false, list: 'complete' }, 'gs'), '');
});

test('rowView shows typed text only while pending', () => {
  const sending = { state: 'sending', value: '60', age_ms: null, status: '' };
  assert.equal(rowView(sending, 2, 2, '90').shown, '90');
  assert.equal(rowView(sending, 2, 2, '90').busy, true);
  const accepted = { state: 'accepted', value: '90', age_ms: 1000, status: '' };
  assert.equal(rowView(accepted, 2, -1, '90').shown, '90');
  const old = { state: 'accepted', value: '90', age_ms: 3500, status: '' };
  assert.equal(rowView(old, 2, -1, null, 0).result, '');
  assert.equal(rowView(accepted, 2, -1, null, 0).result, 'accepted');
  const failed = { state: 'failed', value: '90', age_ms: null, status: '' };
  assert.equal(rowView(failed, 2, -1, null).result, 'failed: refused');
});

test('rowView accepted fades by age_ms plus elapsed time since the push', () => {
  // age_ms is frozen at push time; sincePushMs is how long ago that push
  // landed, on the page's ~1 s re-render clock -- the sum is what ages out.
  const row = { state: 'accepted', value: '90', age_ms: 1000, status: '' };
  assert.equal(rowView(row, 0, -1, null, 2500).result, '');
  assert.equal(rowView(row, 0, -1, null, 1500).result, 'accepted');
});

test('rowView disables other rows while one is pending', () => {
  const row = { state: 'idle', value: '1', age_ms: null, status: '' };
  const v = rowView(row, 5, 4, null);
  assert.equal(v.disabled, true);
  assert.equal(v.busy, false);
  const v2 = rowView(row, 4, 4, null);
  assert.equal(v2.disabled, false);
  assert.equal(v2.busy, true);
  const v3 = rowView(row, 5, -1, null);
  assert.equal(v3.disabled, false);
  assert.equal(v3.busy, false);
});

test('rowView result/resultKind for the remaining states', () => {
  assert.deepEqual(
    (({ result, resultKind }) => ({ result, resultKind }))(
      rowView({ state: 'unsupported', value: '1', age_ms: null, status: 'out of range' }, 0, -1, null)),
    { result: 'unsupported: out of range', resultKind: 'err' });
  assert.deepEqual(
    (({ result, resultKind }) => ({ result, resultKind }))(
      rowView({ state: 'timeout', value: '1', age_ms: null, status: '' }, 0, -1, null)),
    { result: 'no answer', resultKind: 'err' });
  assert.deepEqual(
    (({ result, resultKind }) => ({ result, resultKind }))(
      rowView({ state: 'in_progress', value: '1', age_ms: null, status: '' }, 0, -1, null)),
    { result: 'applying…', resultKind: 'busy' });
  assert.deepEqual(
    (({ result, resultKind }) => ({ result, resultKind }))(
      rowView({ state: 'sending', value: '1', age_ms: null, status: '' }, 0, -1, null)),
    { result: 'sending…', resultKind: 'busy' });
  assert.deepEqual(
    (({ result, resultKind }) => ({ result, resultKind }))(
      rowView({ state: 'idle', value: '1', age_ms: null, status: '' }, 0, -1, null)),
    { result: '', resultKind: '' });
});

test('applyBadge per apply path', () => {
  assert.equal(applyBadge('knob'), 'live');
  assert.equal(applyBadge('reprogram'), '');
  assert.equal(applyBadge('rebuild'), 'restarts encoder ~0.6 s');
});

test('showRefresh only once listing has answered (complete or partial)', () => {
  assert.equal(showRefresh({ list: 'none' }), false);
  assert.equal(showRefresh({ list: 'pending' }), false);
  assert.equal(showRefresh({ list: 'complete' }), true);
  assert.equal(showRefresh({ list: 'partial' }), true);
});

// Drift guard: a renamed or re-sectioned row in the parameter table must
// reach this page's hints and section kickers, or this test fails.
test('HINTS keys and SECTION_KICKER sections name real rows in config_params.h', () => {
  const h = fs.readFileSync(new URL('../../common/include/mabur/config_params.h', import.meta.url), 'utf8');
  const ids = new Set();
  const sections = new Set();
  const re = /\{"([A-Z0-9_]+)",\s*"([a-z_]+)",/g;
  let m;
  while ((m = re.exec(h))) {
    ids.add(m[1]);
    sections.add(m[2]);
  }
  assert.ok(ids.size >= 12, 'parsed the row table');
  for (const key of Object.keys(HINTS)) assert.ok(ids.has(key), `HINTS.${key} is not a row id`);
  for (const section of Object.keys(SECTION_KICKER)) {
    assert.ok(sections.has(section), `SECTION_KICKER.${section} is not a table section`);
  }
});
