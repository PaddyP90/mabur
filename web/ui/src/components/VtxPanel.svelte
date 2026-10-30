<script>
  // The VTX tab: the drone's parameter table (common/include/mabur/
  // config_params.h kTable), pushed by the core as Module.onParams and
  // edited here one row at a time. docs/web-gs.md "VTX tab".
  import { untrack } from 'svelte';
  import { groupRows, rowView, headerText, rowLabel, rowHint, rowWidget, rowShown,
    modeChoices, modeDisplayNote, recSizeChoices, toggleValues, toggleState,
    commitText, commitAnswered, invalidText, showNoHeartbeat } from '../lib/vtxparams.js';

  let { params, paramsAt = null, displayHz = null, spotter = false, variant = 'rule', onSet = () => {} } = $props();
  const uid = $props.id();
  const groupClass = $derived(variant === 'card' ? 'card cardg' : 'group');

  const mode = $derived(spotter ? 'spotter' : 'gs');
  const live = $derived(params ?? { linked: false, rows: [] });
  const rows = $derived(live.rows ?? []);
  const groups = $derived(groupRows(rows));
  const header = $derived(headerText(live, mode));
  // Spotter and "no session yet" both mean nothing here can be edited; the
  // widgets stay in the DOM (disabled) rather than vanishing, so the panel
  // doesn't reflow the moment the link forms.
  const greyed = $derived(spotter || !live.linked);
  const noHeartbeat = $derived(showNoHeartbeat(live, mode));
  // Only one row is ever in flight (the core serialises sets); its index
  // doubles as "something is pending" for every other row's disabled state.
  const pendingIdx = $derived(rows.findIndex((r) => r.state === 'sending' || r.state === 'in_progress'));

  // Typed text per row id: the text just sent, shown in place of a
  // not-yet-updated row.value while sending/in_progress.
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
  function onPick(row, text) { delete localErr[row.id]; commitRow(row, text); }

  // The recorder's settings hide while it reads off; a toggle in flight
  // counts, so turning it on shows them at once.
  const recEnable = $derived.by(() => {
    const r = rows.find((x) => x.id === 'REC_ENABLE');
    return r ? view(r).shown : null;
  });

  // The recorder's size choices follow the camera mode the drone runs.
  const vencLive = $derived(rows.find((r) => r.id === 'VENC_MODE')?.value ?? null);
</script>

<div class="vtx" style="display:flex;flex-direction:column;gap:var(--space-6);font-size:13px;color:var(--color-text)">
  {#if header || noHeartbeat}
    <div class="hdr-row">
      <span class="dim5" style="font-size:12px;flex:1;min-width:0">{header}</span>
      {#if noHeartbeat}<span class="tag tag-neutral" style="font-size:11px">no heartbeat</span>{/if}
    </div>
  {/if}

  <fieldset disabled={greyed} class="fs" style="opacity:{greyed ? 0.5 : 1}">
    {#each groups as g (g.key)}
      <div class={groupClass}>
        <span class="card-kicker">{g.kicker}</span>
        {#each g.rows.filter((r) => rowShown(r, recEnable)) as row (row.id)}
          {@const v = view(row)}
          {@const err = localErr[row.id]}
          {@const off = greyed || v.disabled || v.busy}
          {@const lid = `${uid}${row.id}`}
          {@const w = rowWidget(row)}
          {@const hint = [rowHint(row), w === 'mode' && !spotter ? modeDisplayNote(displayHz) : ''].filter(Boolean).join(' ')}
          <div class="field">
            {#if spotter || w === 'toggle'}
              <span id="{lid}-label" class="fieldlabel">{rowLabel(row)}</span>
            {:else}
              <label for={lid}>{rowLabel(row)}</label>
            {/if}

            {#key epoch[row.id] ?? 0}
              {#if spotter}
                <span class="dim5" aria-labelledby="{lid}-label">—</span>
              {:else if w === 'mode' || w === 'recsize'}
                {@const choices = w === 'mode' ? modeChoices(v.shown, displayHz) : recSizeChoices(vencLive, v.shown)}
                <select id={lid} class="input" disabled={off} onchange={(e) => onPick(row, e.currentTarget.value)}>
                  {#if !v.shown}<option value="" selected disabled>—</option>{/if}
                  {#each choices as c}<option value={c.value} selected={c.value === v.shown}>{c.label}</option>{/each}
                </select>
              {:else if w === 'toggle'}
                {@const t = toggleValues(row)}
                {@const st = toggleState(row, v.shown)}
                <div class="seg" role="radiogroup" aria-labelledby="{lid}-label">
                  <label class="seg-opt"><input type="radio" name={lid} checked={st === 'on'} disabled={off}
                    onchange={() => onPick(row, t.on)}>On</label>
                  <label class="seg-opt"><input type="radio" name={lid} checked={st === 'off'} disabled={off}
                    onchange={() => onPick(row, t.off)}>Off</label>
                </div>
              {:else}
                <input id={lid} class="input" type={row.type === 'str' ? 'text' : 'number'} step={row.type === 'real' ? 'any' : null}
                  value={v.shown} disabled={off}
                  oninput={() => onRowInput(row)} onkeydown={onRowKeydown} onblur={(e) => onRowBlur(row, e)}>
              {/if}
            {/key}

            {#if hint}<div class="hint">{hint}</div>{/if}
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
  .fieldlabel { display: block; font-size: 12px; margin-bottom: 5px; color: color-mix(in srgb, var(--color-text) 70%, transparent); }
  .input { min-height: 34px; padding: 5px 8px; }
  .hint { font-size: 11px; color: var(--color-neutral-500); margin-top: 5px; }
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
  .fs { border: 0; padding: 0; margin: 0; min-width: 0; display: flex; flex-direction: column; gap: var(--space-3); transition: opacity .15s; }
  .vtx :global(.group) { gap: var(--space-4); }
</style>
