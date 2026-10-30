// Pure view logic for the web GS "VTX" tab (no DOM). The core pushes the
// drone's parameter state as JSON via Module.onParams (common/include/
// mabur/config_params.h kTable, 12 rows); VtxPanel.svelte renders what these
// helpers compute. docs/mavlink.md documents the push shape and the apply
// paths (knob/reprogram/rebuild) this file turns into badge text.

// docs/camera-modes.md: sensor field-of-view mode by fps, and the encoder
// sizes that fit it.
export const MODE_SIZES = [
  { maxFps: 90, sizes: ['1920x1080', '1280x720'] },
  { maxFps: 100, sizes: ['1728x972', '1280x720'] },
  { maxFps: 120, sizes: ['1728x816', '1152x544', '864x408'] },
];

export function modeSizes(fps) {
  if (!Number.isInteger(fps) || fps < 1 || fps > 120) return [];
  return MODE_SIZES.find((m) => fps <= m.maxFps).sizes;
}

export function modeText(fps, size) {
  return `${fps}@${size}`;
}

// Mirror of config_params.h parse_mode_text: "<fps>@<WxH>", digits only.
const MODE_RE = /^(\d+)@(\d+x\d+)$/;
export function parseMode(text) {
  const m = MODE_RE.exec(String(text));
  return m ? { fps: Number(m[1]), size: m[2] } : null;
}

// Turns a typed or toggled value into the wire text VENC_MODE etc. expect,
// or null when it cannot hold the drone's type. Returns the canonical text
// (trimmed, not re-formatted) so a round trip through the drone's own value
// reads back the same string the operator typed.
export function commitText(type, input) {
  if (type === 'bool') {
    if (typeof input === 'boolean') return input ? '1' : '0';
    if (typeof input !== 'string') return null;
    const s = input.trim();
    if (s === '1' || s === 'on' || s === 'true') return '1';
    if (s === '0' || s === 'off' || s === 'false') return '0';
    return null;
  }
  const s = String(input).trim();
  if (type === 'int') return /^-?\d+$/.test(s) ? s : null;
  if (type === 'real') {
    if (s === '') return null;
    return Number.isFinite(Number(s)) ? s : null;
  }
  if (type === 'str') {
    if (s.length < 1 || s.length > 127) return null;
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c < 0x20 || c > 0x7e) return null;
    }
    return s.includes('"') || s.includes('\\') ? null : s;
  }
  return null;
}

// The tab's section order and headings.
export const SECTION_KICKER = {
  radio: 'Radio',
  venc: 'Camera',
  encoder: 'Encoder',
  record: 'Recorder',
  low_power: 'Power',
  msp: 'Flight controller',
};

// Groups the pushed rows by section, in table order (first appearance order,
// since the core always pushes rows in kTable order already).
export function groupRows(rows) {
  const groups = [];
  const bySection = new Map();
  for (const row of rows) {
    let g = bySection.get(row.section);
    if (!g) {
      g = { key: row.section, kicker: SECTION_KICKER[row.section] ?? row.section, rows: [] };
      bySection.set(row.section, g);
      groups.push(g);
    }
    g.rows.push(row);
  }
  return groups;
}

// One sentence next to the rows this knob interacts with.
export const HINTS = {
  VENC_MODE: 'A change of aspect also sets the recording size to the new camera size',
  REC_FPS: 'At most the camera fps',
  REC_SIZE: 'Same aspect as the camera size; above 1080p needs ≤ 30 fps',
  LOWPWR_ENABLE: 'Needs MSP on',
  MSP_ENABLE: 'Turn low power off first',
};

// docs/mavlink.md "Applying a set": a knob takes effect at once with
// nothing on the row to show for it; a reprogram changes a radio or
// peripheral setting in place and is badged "live"; a rebuild stops and
// restarts the encoder pipeline (about 0.5 s + ~0.1 s to first frame).
export function applyBadge(apply) {
  if (apply === 'reprogram') return 'live';
  if (apply === 'rebuild') return 'restarts encoder ~0.6 s';
  return '';
}

export function headerText(params, mode) {
  if (mode === 'spotter') return 'VTX settings need GS mode.';
  if (!params.linked) return 'Waiting for the drone link…';
  if (params.mismatch) return "The drone's parameter table differs from this page — redeploy.";
  if (params.list === 'pending') return "Reading the drone's settings…";
  if (params.list === 'partial') return 'Some settings did not answer.';
  return '';
}

// One row's render: the widget always shows the drone's value, never the
// typed text, once a terminal state arrives -- only while Sending/InProgress
// does it keep showing what the operator typed. `accepted` fades 3 s after
// the push that carried it; age_ms is frozen at push time, so the caller
// (on a ~1 s clock) passes how long ago that push arrived.
export function rowView(row, idx, pending, typed, sincePushMs = 0) {
  const busy = pending === idx;
  const disabled = pending !== -1 && !busy;
  const sendingLike = row.state === 'sending' || row.state === 'in_progress';
  const shown = sendingLike && typed != null ? typed : row.value ?? '';
  let result = '';
  let resultKind = '';
  switch (row.state) {
    case 'accepted':
      if ((row.age_ms ?? 0) + sincePushMs < 3000) {
        result = 'accepted';
        resultKind = 'ok';
      }
      break;
    case 'failed':
      result = `failed: ${row.status || 'refused'}`;
      resultKind = 'err';
      break;
    case 'unsupported':
      result = `unsupported: ${row.status}`;
      resultKind = 'err';
      break;
    case 'timeout':
      result = 'no answer';
      resultKind = 'err';
      break;
    case 'in_progress':
      result = 'applying…';
      resultKind = 'busy';
      break;
    case 'sending':
      result = 'sending…';
      resultKind = 'busy';
      break;
    default:
      break;
  }
  return { shown, disabled, busy, result, resultKind };
}

export function showRefresh(params) {
  return params.list === 'complete' || params.list === 'partial';
}
