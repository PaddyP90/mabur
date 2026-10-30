<script>
  // The VTX tab: the drone's parameter table (common/include/mabur/
  // config_params.h kTable), pushed by the core as Module.onParams and
  // edited here one row at a time. docs/mavlink.md "Later tenants" /
  // docs/web-gs.md "VTX tab".
  import { untrack } from 'svelte';
  import { groupRows, rowView, showRefresh, headerText, applyBadge, HINTS,
    modeSizes, modeText, parseMode, commitText } from '../lib/vtxparams.js';

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
  const noHeartbeat = $derived(!spotter && !!live.linked && !live.alive);
  // Only one row is ever in flight (the core serialises sets); its index
  // doubles as "something is pending" for every other row's disabled state.
  const pendingIdx = $derived(rows.findIndex((r) => r.state === 'sending' || r.state === 'in_progress'));

  // Typed text per row id: the text just sent, kept visible in place of a
  // not-yet-updated row.value while sending/in_progress, cleared once the
  // row lands on a terminal state (or idle) so the drone's own value shows
  // again. VENC_MODE also uses this key, carrying the committed "fps@WxH".
  let typed = $state({});
  let prevRowState = $state({});
  $effect(() => {
    for (const row of rows) {
      const was = prevRowState[row.id];
      const stillPending = row.state === 'sending' || row.state === 'in_progress';
      if ((was === 'sending' || was === 'in_progress') && !stillPending) delete typed[row.id];
      prevRowState[row.id] = row.state;
    }
  });

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
    const text = commitText(row.type, raw);
    if (text == null || text === row.value) return;   // null = can't fix it up, keep the typed text as-is
    typed[row.id] = text;
    onSet(row.id, text);
  }
  function onRowKeydown(e) { if (e.key === 'Enter') e.currentTarget.blur(); }
  function onRowBlur(row, e) { commitRow(row, e.currentTarget.value); }
  function onBoolChange(row, e) { commitRow(row, e.currentTarget.checked); }

  // VENC_MODE: fps + size edited as a pair, applied together as one
  // "<fps>@<WxH>" set (docs/camera-modes.md). `draft` is the pair the
  // operator is composing; it re-syncs to the drone's value only when the
  // row leaves sending/in_progress, so mid-edit it is never clobbered by
  // an unrelated params push.
  const vencRow = $derived(rows.find((r) => r.id === 'VENC_MODE'));
  // Seeded once from the prop directly (never from the `rows` derived --
  // that would only capture its value at this line's first run, not track
  // it), then kept in step by the resync effect below.
  let draft = $state(untrack(() => parseMode((params?.rows ?? []).find((r) => r.id === 'VENC_MODE')?.value))
    ?? { fps: 90, size: '1920x1080' });
  let prevVencState = null;
  $effect(() => {
    if (!vencRow) return;
    const wasPending = prevVencState === 'sending' || prevVencState === 'in_progress';
    const stillPending = vencRow.state === 'sending' || vencRow.state === 'in_progress';
    if (wasPending && !stillPending) draft = parseMode(vencRow.value) ?? draft;
    prevVencState = vencRow.state;
  });
  const vencSizes = $derived(modeSizes(draft.fps));
  const vencText = $derived(modeText(draft.fps, draft.size));
  function onFpsInput(e) {
    const n = Math.max(1, Math.min(120, Math.trunc(Number(e.currentTarget.value)) || 1));
    const sizes = modeSizes(n);
    draft = { fps: n, size: sizes.includes(draft.size) ? draft.size : sizes[0] };
  }
  function onSizeChange(e) { draft = { ...draft, size: e.currentTarget.value }; }
  function applyVenc() {
    if (!vencRow || vencText === vencRow.value) return;
    typed[vencRow.id] = vencText;
    onSet(vencRow.id, vencText);
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
          <div class="field">
            <div class="rowhead">
              <span class="fieldlabel" id="{uid}{row.id}-label" title={row.help}>{row.key}</span>
              {#if applyBadge(row.apply)}<span class="tag tag-neutral badge">{applyBadge(row.apply)}</span>{/if}
            </div>

            {#if spotter}
              <span class="dim5 dash" aria-labelledby="{uid}{row.id}-label">—</span>
            {:else if row.id === 'VENC_MODE'}
              <div class="mode-row">
                <input class="input sm num" type="number" min="1" max="120" style="width:64px"
                  aria-label="{row.key} frame rate" value={draft.fps}
                  disabled={greyed || v.disabled || v.busy} oninput={onFpsInput}>
                <select class="input sm" aria-label="{row.key} size" value={draft.size}
                  disabled={greyed || v.disabled || v.busy} onchange={onSizeChange}>
                  {#each vencSizes as s}<option value={s} selected={s === draft.size}>{s}</option>{/each}
                </select>
                <button class="btn btn-secondary" type="button" style="font-size:12px"
                  disabled={greyed || v.disabled || v.busy || vencText === (vencRow?.value ?? null)}
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
                onkeydown={onRowKeydown} onblur={(e) => onRowBlur(row, e)}>
            {/if}

            {#if HINTS[row.id]}<div class="hint">{HINTS[row.id]}</div>{/if}
            {#if !spotter && v.result}
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
  .fieldlabel { font-size: 12px; color: color-mix(in srgb, var(--color-text) 70%, transparent); cursor: help; }
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
