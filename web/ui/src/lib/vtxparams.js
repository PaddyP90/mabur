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

// The fps box's text as a frame rate: a plain integer 1..120, else null.
// Never clamped -- the box keeps exactly what the operator typed.
export function parseFps(text) {
  const s = String(text ?? '').trim();
  if (!/^\d{1,3}$/.test(s)) return null;
  const n = Number(s);
  return n >= 1 && n <= 120 ? n : null;
}

// The size options for `fps`: modeSizes(fps), plus the drone's own size
// when it runs one outside that list in the same sensor mode (an ssh-set
// custom size), so a fps-only change keeps it rather than silently
// swapping in the mode's first size. Appended, so crossing into this mode
// from another still lands on the mode's first standard size.
export function modeOptions(fps, liveValue) {
  const sizes = modeSizes(fps);
  const live = parseMode(liveValue);
  if (!live || sizes.length === 0 || modeSizes(live.fps) !== sizes || sizes.includes(live.size)) return sizes;
  return [...sizes, live.size];
}

export function modeText(fps, size) {
  return `${fps}@${size}`;
}

// Mirror of config_params.h parse_mode_text: "<fps>@<WxH>", each number a
// run of 1..6 digits.
const MODE_RE = /^(\d{1,6})@(\d{1,6}x\d{1,6})$/;
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

// The VENC_MODE row's draft: the fps box's raw text and the chosen size.
// Empty until the drone's value is known -- never a made-up default an
// Apply could send.
export function modeDraftFrom(value) {
  const m = parseMode(value);
  return m ? { fpsText: String(m.fps), size: m.size } : { fpsText: '', size: '' };
}

function sameDraft(a, b) {
  return a.fpsText === b.fpsText && a.size === b.size;
}

// One step of the VENC_MODE draft on a params push. st = { draft, seen,
// pending }: `seen` is the drone value the draft was last synced from,
// `pending` whether the row was sending/in_progress at the last step.
// The draft follows the drone's value whenever that changes and the
// operator hasn't edited it (it still equals what `seen` gives), and
// always once a set of this row leaves sending/in_progress; while a set
// is in flight it is left alone. Returns `st` itself when nothing changed.
export function modeDraftNext(st, value, state) {
  const pending = state === 'sending' || state === 'in_progress';
  if (pending) return st.pending ? st : { ...st, pending: true };
  const v = value ?? null;
  if (st.pending) return { draft: modeDraftFrom(v), seen: v, pending: false };
  if (v === st.seen) return st;
  const clean = sameDraft(st.draft, modeDraftFrom(st.seen));
  return { draft: clean ? modeDraftFrom(v) : st.draft, seen: v, pending: false };
}

// The "<fps>@<WxH>" an Apply would send, or null while Apply must stay
// disabled: the fps text isn't an integer 1..120, no size is chosen, or
// the pair is already the drone's live value.
export function modeApplyText(draft, liveValue) {
  const fps = parseFps(draft.fpsText);
  if (fps == null || !draft.size) return null;
  const text = modeText(fps, draft.size);
  return text === liveValue ? null : text;
}

// The result line for text the page could not turn into the row's type
// (commitText returned null): shown locally, nothing is sent.
export function invalidText(type) {
  if (type === 'int') return 'not a valid integer';
  if (type === 'real') return 'not a valid number';
  if (type === 'str') return 'not valid text: 1–127 printable characters, no " or \\';
  return 'not a valid value';
}

// The row label's dim config-key half. VENC_MODE spells two keys.
export function rowKeys(row) {
  if (row.id === 'VENC_MODE') return 'venc.fps + venc.size';
  return `${row.section}.${row.key}`;
}

// The "no heartbeat" tag: GS mode, linked, and the drone's heartbeat was
// seen on this link and has since gone quiet (alive is null before the
// first one, which is no verdict yet).
export function showNoHeartbeat(params, mode) {
  return mode !== 'spotter' && !!params.linked && params.alive === false;
}

// Whether the answer to a commit has arrived. `snap` is the row as the
// last push showed it when the commit went out ({ state, age_ms, status,
// sawPending }); the caller sets sawPending once a push shows the row
// sending/in_progress. Answered: the row went through sending/in_progress
// and left it, or it was refused at once -- a terminal result newer than
// the snapshot's (another state or status, or an age_ms that restarted).
export function commitAnswered(snap, row) {
  if (row.state === 'sending' || row.state === 'in_progress') return false;
  if (snap.sawPending) return true;
  if (row.state !== snap.state || (row.status ?? '') !== (snap.status ?? '')) return true;
  if (row.age_ms == null) return false;
  return snap.age_ms == null || row.age_ms < snap.age_ms;
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
      // VALUE_UNSUPPORTED carries no STATUSTEXT: the drone refused the
      // value's type or shape (or the id) without a reason line.
      result = `unsupported: ${row.status || "the drone refused this value's shape"}`;
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
