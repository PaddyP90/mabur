<script>
  // The VTX tab: the drone's parameter table (common/include/mabur/
  // config_params.h kTable), pushed by the core as Module.onParams and
  // edited here one row at a time. docs/web-gs.md "VTX tab".
  import { untrack } from 'svelte';
  import { groupRows, rowView, showRefresh, headerText, applyBadge, HINTS,
    modeOptions, parseFps, modeDraftFrom, modeDraftNext, modeApplyText,
    commitText, commitAnswered, invalidText, rowKeys, showNoHeartbeat } from '../lib/vtxparams.js';

  let { params, paramsAt = null, spotter = false, onSet = () => {}, onRefresh = () => {} } = $props();
  const uid = $props.id();

  const mode = $derived(spotter ? 'spotter' : 'gs');
  const live = $derived(params ?? { linked: false, rows: [] });
  const rows = $derived(live.rows ?? []);
  const groups = $derived(groupRows(rows));
  const header = $derived(headerText(live, mode));
  // Spotter and "no session yet" both mean nothing here can be edited; the
  // widgets stay in the DOM (disabled) rather than vanishing, so the panel
  // doesn't reflow the moment the link forms.
  const greyed = $derived(spotter || !live.linked);
  const refreshShown = $derived(!spotter && showRefresh(live));
  const noHeartbeat = $derived(showNoHeartbeat(live, mode));
  // Only one row is ever in flight (the core serialises sets); its index
  // doubles as "something is pending" for every other row's disabled state.
  const pendingIdx = $derived(rows.findIndex((r) => r.state === 'sending' || r.state === 'in_progress'));

  // Typed text per row id: the text just sent, shown in place of a
  // not-yet-updated row.value while sending/in_progress. VENC_MODE also
  // uses this key, carrying the committed "fps@WxH".
  let typed = $state({});
  // Per row id: a not-sendable edit's local result line ("not a valid
  // ..."), shown until that row's next edit.
  let localErr = $state({});
  // Per row id: bumped when a commit's answer arrives, keying the row's
  // widget so it is rebuilt on the drone's value. The browser keeps
  // whatever the operator typed or toggled in the element, and Svelte only
  // rewrites it when the bound expression changes -- an immediate refusal
  // leaves row.value unchanged, so without this the widget would keep the
  // refused input next to "failed: ...".
  let epoch = $state({});
  // Per row id: the row as the last push showed it when a commit went out
  // (commitAnswered's snapshot). Not reactive: only the effect reads it.
  const awaiting = {};
  $effect(() => {
    for (const row of rows) {
      const snap = awaiting[row.id];
      if (!snap) continue;
      if (row.state === 'sending' || row.state === 'in_progress') { snap.sawPending = true; continue; }
      if (!commitAnswered(snap, row)) continue;
      delete awaiting[row.id];
      delete typed[row.id];
      epoch[row.id] = untrack(() => epoch[row.id] ?? 0) + 1;
      if (row.id === 'VENC_MODE') md = { draft: modeDraftFrom(row.value), seen: row.value ?? null, pending: false };
    }
  });
  function sent(row, text) {
    awaiting[row.id] = { state: row.state, age_ms: row.age_ms ?? null, status: row.status ?? '', sawPending: false };
    typed[row.id] = text;
    onSet(row.id, text);
  }

  // A ~1 s clock, running only while a row is fading out an "accepted"
  // result -- rowView's age_ms is frozen at push time, so this is what
  // ages it past 3 s without a new push ever arriving.
  let tickAt = $state(Date.now());
  let tickTimer = null;
  $effect(() => {
    const need = rows.some((r) => r.state === 'accepted');
    if (need && !tickTimer) tickTimer = setInterval(() => { tickAt = Date.now(); }, 1000);
    else if (!need && tickTimer) { clearInterval(tickTimer); tickTimer = null; }
  });
  $effect(() => () => { if (tickTimer) clearInterval(tickTimer); });
  const sincePushMs = $derived(Math.max(0, tickAt - (paramsAt ?? tickAt)));

  function view(row) {
    return rowView(row, rows.indexOf(row), pendingIdx, typed[row.id] ?? null, sincePushMs);
  }

  function commitRow(row, raw) {
    // Nothing edited (e.g. focus passing through an empty, unanswered row).
    if (typeof raw === 'string' && raw.trim() === (row.value ?? '')) return;
    const text = commitText(row.type, raw);
    if (text == null) { localErr[row.id] = invalidText(row.type); return; }
    if (text === row.value) return;
    sent(row, text);
  }
  function onRowInput(row) { delete localErr[row.id]; }
  function onRowKeydown(e) { if (e.key === 'Enter') e.currentTarget.blur(); }
  function onRowBlur(row, e) { commitRow(row, e.currentTarget.value); }
  function onBoolChange(row, e) { delete localErr[row.id]; commitRow(row, e.currentTarget.checked); }

  // VENC_MODE: fps + size edited as a pair, applied together as one
  // "<fps>@<WxH>" set (docs/camera-modes.md). `md.draft` is the pair the
  // operator is composing, the fps as the raw text in its box; it follows
  // the drone's value until the operator edits it (modeDraftNext), and is
  // never touched by a push while a set of it is in flight.
  const vencRow = $derived(rows.find((r) => r.id === 'VENC_MODE'));
  const vencLive = $derived(vencRow?.value ?? null);
  let md = $state({ draft: modeDraftFrom(null), seen: null, pending: false });
  $effect(() => {
    if (!vencRow) return;
    const prev = untrack(() => md);
    const next = modeDraftNext(prev, vencRow.value, vencRow.state);
    if (next !== prev) md = next;
  });
  const vencFps = $derived(parseFps(md.draft.fpsText));
  // Sizes for the typed fps; while that text is not a valid rate, the
  // chosen size stays the only option rather than the list emptying.
  const vencSizes = $derived(vencFps == null ? (md.draft.size ? [md.draft.size] : []) : modeOptions(vencFps, vencLive));
  const vencApply = $derived(modeApplyText(md.draft, vencLive));
  function onFpsInput(e) {
    const fpsText = e.currentTarget.value;
    const n = parseFps(fpsText);
    let size = md.draft.size;
    if (n != null) {
      const sizes = modeOptions(n, vencLive);
      if (!sizes.includes(size)) size = sizes[0];
    }
    md = { ...md, draft: { fpsText, size } };
  }
  function onSizeChange(e) { md = { ...md, draft: { ...md.draft, size: e.currentTarget.value } }; }
  function applyVenc() {
    if (!vencRow || vencApply == null) return;
    sent(vencRow, vencApply);
  }
</script>

<div class="vtx" style="display:flex;flex-direction:column;gap:var(--space-4);font-size:13px;color:var(--color-text)">
  <div class="hdr-row">
    <span class="dim5" style="font-size:12px;flex:1;min-width:0">{header}</span>
    {#if noHeartbeat}<span class="tag tag-neutral" style="font-size:11px">no heartbeat</span>{/if}
    {#if refreshShown}
      <button class="btn btn-ghost" type="button" style="font-size:12px" disabled={pendingIdx !== -1} onclick={onRefresh}>Refresh</button>
    {/if}
  </div>

  <fieldset disabled={greyed} class="fs" style="opacity:{greyed ? 0.5 : 1}">
    {#each groups as g (g.key)}
      <div class="group">
        <span class="card-kicker">{g.kicker}</span>
        {#each g.rows as row (row.id)}
          {@const v = view(row)}
          {@const err = localErr[row.id]}
          <div class="field">
            <div class="rowhead">
              <span class="fieldlabel" id="{uid}{row.id}-label">
                <span class="mono rowid">{row.id}</span>
                <span class="rowkey">{rowKeys(row)}</span>
              </span>
              {#if applyBadge(row.apply)}<span class="tag tag-neutral badge">{applyBadge(row.apply)}</span>{/if}
            </div>

            {#key epoch[row.id] ?? 0}
              {#if spotter}
                <span class="dim5 dash" aria-labelledby="{uid}{row.id}-label">—</span>
              {:else if row.id === 'VENC_MODE'}
                <div class="mode-row">
                  <!-- type="text": a number input reports "" for half-typed text, and
                       writing that back would wipe what the operator typed. -->
                  <input class="input sm num" type="text" inputmode="numeric" style="width:64px"
                    aria-label="{row.id} frame rate, 1 to 120" value={md.draft.fpsText}
                    aria-invalid={md.draft.fpsText !== '' && vencFps == null}
                    disabled={greyed || v.disabled || v.busy} oninput={onFpsInput}>
                  <select class="input sm" aria-label="{row.id} size" value={md.draft.size}
                    disabled={greyed || v.disabled || v.busy} onchange={onSizeChange}>
                    {#each vencSizes as s}<option value={s} selected={s === md.draft.size}>{s}</option>{/each}
                  </select>
                  <button class="btn btn-secondary" type="button" style="font-size:12px"
                    disabled={greyed || v.disabled || v.busy || vencApply == null}
                    onclick={applyVenc}>
                    {#if v.busy}<span class="spinner" aria-hidden="true"></span>applying…{:else}Apply{/if}
                  </button>
                </div>
              {:else if row.type === 'bool'}
                <label class="vswitch" aria-labelledby="{uid}{row.id}-label">
                  <input type="checkbox" checked={v.shown === '1'}
                    disabled={greyed || v.disabled || v.busy} onchange={(e) => onBoolChange(row, e)}>
                  <span class="track"><span class="thumb"></span></span>
                </label>
              {:else}
                <input class="input sm" type={row.type === 'str' ? 'text' : 'number'} step={row.type === 'real' ? 'any' : null}
                  aria-labelledby="{uid}{row.id}-label" value={v.shown}
                  disabled={greyed || v.disabled || v.busy}
                  oninput={() => onRowInput(row)} onkeydown={onRowKeydown} onblur={(e) => onRowBlur(row, e)}>
              {/if}
            {/key}

            <div class="hint">{row.help}</div>
            {#if HINTS[row.id]}<div class="hint">{HINTS[row.id]}</div>{/if}
            {#if !spotter && err}
              <div class="result result-err">{err}</div>
            {:else if !spotter && v.result}
              <div class="result result-{v.resultKind}">
                {#if v.resultKind === 'busy'}<span class="spinner" aria-hidden="true"></span>{/if}{v.result}
              </div>
            {/if}
          </div>
        {/each}
      </div>
    {/each}
  </fieldset>
</div>

<style>
  .hdr-row { display: flex; align-items: center; gap: 8px; }
  .dash { display: inline-block; font-size: 13px; }
  .rowhead { display: flex; align-items: center; justify-content: space-between; gap: 6px; margin-bottom: 5px; }
  .fieldlabel { font-size: 12px; display: flex; flex-wrap: wrap; align-items: baseline; gap: 2px 8px; min-width: 0; }
  .rowid { color: color-mix(in srgb, var(--color-text) 85%, transparent); }
  .rowkey { font-size: 11px; color: var(--color-neutral-500); }
  .badge { font-size: 10px; padding: 1px 6px; }
  .hint { font-size: 11px; color: var(--color-neutral-500); margin-top: 4px; }
  .result { font-size: 11px; margin-top: 4px; }
  .result-ok { color: var(--color-accent); }
  .result-err { color: var(--color-status-critical); }
  .result-busy { color: var(--color-neutral-500); }
  .spinner { display: inline-block; width: 9px; height: 9px; margin-right: 5px; vertical-align: -1px;
    border-radius: 50%; border: 2px solid var(--color-neutral-700); border-top-color: var(--color-neutral-300);
    animation: vspin .7s linear infinite; }
  @keyframes vspin { to { transform: rotate(360deg); } }
  @media (prefers-reduced-motion: reduce) {
    .spinner { animation: none; }
  }
  .mode-row { display: flex; gap: 6px; align-items: center; }
  .mode-row select { flex: 1; min-width: 0; }
  .fs { border: 0; padding: 0; margin: 0; min-width: 0; display: flex; flex-direction: column; gap: var(--space-4); transition: opacity .15s; }
  .vtx :global(.group) { gap: var(--space-3); }

  /* A two-state switch, since there is no bare checkbox in this theme. */
  .vswitch { position: relative; display: inline-flex; width: 36px; height: 20px; cursor: pointer; }
  .vswitch input { position: absolute; inset: 0; margin: 0; opacity: 0; cursor: pointer; }
  .vswitch .track { position: absolute; inset: 0; border-radius: 999px; background: var(--color-neutral-800);
    box-shadow: inset 0 0 0 1px var(--color-divider); transition: background .12s; }
  .vswitch .thumb { position: absolute; top: 2px; left: 2px; width: 16px; height: 16px; border-radius: 50%;
    background: var(--color-neutral-400); transition: transform .12s, background .12s; }
  .vswitch input:checked + .track { background: var(--color-accent-900); box-shadow: inset 0 0 0 1px var(--color-accent); }
  .vswitch input:checked + .track .thumb { transform: translateX(16px); background: var(--color-accent-200); }
  .vswitch input:disabled + .track { opacity: .5; cursor: default; }
  .vswitch input:focus-visible + .track { outline: 2px solid var(--color-accent); outline-offset: 2px; }
</style>
