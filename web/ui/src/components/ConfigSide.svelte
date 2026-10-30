<script>
  import Icon from './Icon.svelte';
  let { onClose, tab, onTab, config, vtx } = $props();
  const subtitle = $derived(tab === 'vtx' ? 'Live drone settings' : 'Editable while disconnected');
</script>
<div class="side glass-panel">
  <div style="display:flex;align-items:center;gap:8px;padding:var(--space-4) var(--space-3) var(--space-2) var(--space-4)">
    <div style="display:flex;flex-direction:column;gap:4px;margin-right:auto;min-width:0">
      <div class="seg" style="flex:none">
        <label class="seg-opt"><input type="radio" name="cfgside-tab" checked={tab === 'config'} onchange={() => onTab('config')}><Icon name="sliders-horizontal" />Config</label>
        <label class="seg-opt"><input type="radio" name="cfgside-tab" checked={tab === 'vtx'} onchange={() => onTab('vtx')}><Icon name="broadcast" />VTX</label>
      </div>
      <span class="dim4" style="font-size:12px">{subtitle}</span>
    </div>
    <button class="btn btn-ghost" type="button" title="Close" aria-label="Close configuration" style="width:32px;height:32px;padding:0;align-self:flex-start;margin-top:2px" onclick={onClose}><Icon name="x" size="16px" /></button>
  </div>
  <div class="body scroll-thin">{#if tab === 'vtx'}{@render vtx?.()}{:else}{@render config?.()}{/if}</div>
</div>
<style>
  .side { position: absolute; top: 64px; right: 12px; bottom: 12px; width: min(420px, calc(100% - 24px)); display: flex; flex-direction: column; overflow: hidden; z-index: 4; }
  .body { flex: 1; min-height: 0; overflow-y: auto; overflow-x: hidden; padding: var(--space-2) var(--space-4) var(--space-6); }
</style>
