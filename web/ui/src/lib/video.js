// Decode + draw (port of the old web/www/app.js onAu/makeDecoder). Owns the
// IRAP gate, the WebCodecs decoder, the canvas draw, the page-side latency
// segments and the 200 ms page metrics. Never touched by Svelte per frame.
import {
  Gate, PtsUnwrap, PeriodEstimator, HitchMeter, annexbToLengthPrefixed, DecoderSlot, SegWindow,
  capToGlass, pruneSubmitted, trimBefore, isStalePresentSample, IdrRequester,
} from './logic.mjs';
import { PhaseController } from './phase.js';
import { PageMetrics } from './metrics.js';
import { ColorTransGl } from './colortrans.js';

const now = () => performance.timeOrigin + performance.now();

export class VideoPipeline {
  // getCanvas: the flat 2D canvas; getGlCanvas: the colortrans WebGL one
  // (a canvas cannot change context type). getColortrans: the live toggle.
  // getModule/getLowPower feed the phase-lock controller (GS mode only).
  constructor({
    getCanvas, getGlCanvas = () => null, getMode, getColortrans = () => false, onWantIdr = () => {},
    getModule = () => null, getLowPower = () => null,
  }) {
    this.getCanvas = getCanvas;
    this.getGlCanvas = getGlCanvas;
    this.getMode = getMode;
    this.getColortrans = getColortrans;
    this.onWantIdr = onWantIdr;
    // Page lifetime, like the canvases: built on the first colortrans frame;
    // a failure (no WebGL, lost context) falls back to flat for good.
    this.ct = null;
    this.ctFailed = false;
    this.shown = null;
    // ONE slot for the page lifetime: every replace() closes the decoder it
    // held, so a Connect never leaks the previous session's hardware decoder.
    this.decoderSlot = new DecoderSlot();
    this.noCodecWarned = false;
    // Page lifetime too: reset()/close() flush and stop it rather than
    // replacing it, so swapping `this.phase` for a fake (tests) takes
    // effect at every call site, and its `enabled` preference survives
    // every Connect.
    // onEvicted is the controller's only path back to us when it drops
    // queued frames with no caller of its own to hand them to (disable,
    // a transit jump, a hidden tab the tick timer catches). A spotter has
    // nothing to phase-lock: the controller resets itself on every
    // VideoPipeline reset() and has no notion of "mode", so starving it of
    // the module here is what keeps a spotter session from ever making a
    // real _webgs_phase_* call.
    const spotterSafeModule = () => (this.getMode() === 'gs' ? getModule() : null);
    this.phase = new PhaseController({ getModule: spotterSafeModule, getLowPower, onEvicted: (es) => this._closeEntries(es) });
    this._heldMeta = new Map();   // held VideoFrame -> { rec, tOutRaw }
    this._rafHandle = null;
    this._phaseLive = false;
    this._onVisibilityChange = null;
    this.reset();
  }

  // The one place that closes a held/displaced VideoFrame: drops its
  // _heldMeta record first so a frame the controller already evicted
  // can never be mistaken for one still pending. Tolerates a nullish
  // list (a test fake's flush()/reset() forgetting to return one).
  _closeEntries(entries) {
    for (const e of entries || []) {
      this._heldMeta.delete(e.frame);
      e.frame.close();
    }
  }

  // Stops the continuous rAF loop and the controller's tick interval, and
  // closes whatever it was still holding -- neither a disconnect nor a
  // fresh Connect may leak a VideoFrame into the next session.
  _flushPhase() {
    this._stopPhaseLoop();
    this._closeEntries(this.phase.flush());
    this._heldMeta.clear();
    this._phaseLive = false;
  }

  // Started lazily on the first onAu in GS mode: a spotter never decodes
  // its own picture to present, so it has nothing to phase-lock and never
  // calls this -- no ticks, no rAF loop, no hold.
  _startPhaseLoop() {
    this.phase.start();
    // Chrome throttles the controller's own 50 ms tick to >= 1 s once the
    // tab is hidden, and the rAF loop below simply stops -- neither would
    // notice a hidden tab quickly enough on its own. This is the fast path.
    this._onVisibilityChange = () => {
      if (document.hidden) this._closeEntries(this.phase.flush());
    };
    document.addEventListener('visibilitychange', this._onVisibilityChange);
    const loop = (ts) => {
      this._rafHandle = requestAnimationFrame(loop);
      const { draw, displaced } = this.phase.onRaf(ts);
      this._closeEntries(displaced);
      if (draw) this._presentHeld(draw);
    };
    this._rafHandle = requestAnimationFrame(loop);
  }

  _stopPhaseLoop() {
    if (this._rafHandle != null) { cancelAnimationFrame(this._rafHandle); this._rafHandle = null; }
    if (this._onVisibilityChange) {
      document.removeEventListener('visibilitychange', this._onVisibilityChange);
      this._onVisibilityChange = null;
    }
    this.phase.stop();
  }

  // Draws a frame the controller released at this rAF's tick and takes
  // its deferred 'present' / 'capture->glass (GS)' samples at that same
  // tick, instead of the next-rAF sample an immediately drawn frame gets.
  _presentHeld({ frame, tick }) {
    const meta = this._heldMeta.get(frame);
    this._heldMeta.delete(frame);
    this.draw(frame);
    frame.close();
    if (!meta) return;
    this.segWindow.add('present', tick - meta.tOutRaw);
    const rec = meta.rec;
    if (rec && this.getMode() === 'gs') {
      const tPresentMs = tick + performance.timeOrigin;
      const cap = capToGlass({ capToCompleteUs: rec.capUs, tEmitMs: rec.tEmitMs,
        tRecvMs: rec.tRecvMs, tPresentMs });
      if (cap != null) this.segWindow.add('capture→glass (GS)', cap);
    }
  }

  reset() {
    // Keyed on whether the loop is running, not on getMode(): at a
    // Connect's reset() the session still reports the previous session's
    // mode. The controller's own reset() always runs (pure JS; a spotter's
    // spotterSafeModule hands it no module, so it makes no wasm call) so
    // the next GS session starts from a fresh grid, window and counters.
    if (this._phaseLive) this._flushPhase();
    this._closeEntries(this.phase.reset());
    this.gate = new Gate();
    this.idr = new IdrRequester();
    this.unwrap = new PtsUnwrap();
    this.period = new PeriodEstimator();
    this.hitch = new HitchMeter();
    this.submitted = new Map();
    // Created lazily on the first key frame (onAu): a browser without
    // WebCodecs must still load the page.
    this.decoder = this.decoderSlot.replace(null);
    this.hvcc = null;
    this.configuredWith = null;
    // Set when a 'key' chunk is submitted to the decoder, cleared by the
    // output callback (a real decode happened) or an error path (the
    // failure is charged to IdrRequester's backoff instead).
    this.keyPending = false;
    this.segWindow = new SegWindow(now);
    this.metrics = new PageMetrics();
    this.hitchTimes = [];
    this.hitchesTotal = 0;
    this.videoSize = null;
    this.colour = null;   // 'colortrans' | 'flat' | 'unavailable', per last drawn frame
    // A frame has been decoded since this Connect. Until then the screen is
    // black and the page says what it is waiting for; after it, a freeze
    // keeps the last frame up with nothing drawn over it.
    this.hasPicture = false;
  }

  gateArmed() { return this.gate.armed; }
  periodMs() { return this.period.periodMs(); }

  // A replaced decoder never outputs its queued chunks: drop their records.
  // The next key frame builds a fresh one (onAu).
  replaceDecoder() {
    this.submitted.clear();
    this.keyPending = false;
    this.decoder = this.decoderSlot.replace(null);
  }

  makeDecoder() {
    return new VideoDecoder({
      output: (frame) => {
        // A real decode happened: the key frame that started this run (if
        // any) succeeded, and IdrRequester's backoff resets.
        this.keyPending = false;
        this.idr.noteOutput();
        const t = now();
        const rec = this.submitted.get(frame.timestamp);
        if (rec) {
          this.segWindow.add('decode', t - rec.tSubmit);
          this.submitted.delete(frame.timestamp);
        }
        this.videoSize = { w: frame.displayWidth, h: frame.displayHeight };
        this.hasPicture = true;

        let held = false;
        if (this.getMode() === 'gs') {
          const tOutRaw = performance.now();
          if (this.phase.onFrame({ frame, ptsUs: frame.timestamp, tOut: tOutRaw }) === 'hold') {
            held = true;
            this._heldMeta.set(frame, { rec, tOutRaw });
          } else {
            // 'now', for any reason (lock dropped, mismatch, disabled,
            // hidden, late, a refused push, a bad >3-period estimate): an
            // older held frame must not outlive this one by sitting in the
            // queue until the rAF loop draws it later, over this one.
            this._closeEntries(this.phase.displaceQueued());
          }
        }
        if (!held) {
          this.draw(frame);
          frame.close();
        }

        this.metrics.addDraw(t);
        const gap = this.hitch.addDraw(t, this.period.periodMs());
        if (gap !== null) {
          this.hitchTimes.push(t);
          trimBefore(this.hitchTimes, t - 60000);
          this.hitchesTotal++;
        }
        // Held: drawn later at its release tick, with its own 'present' /
        // 'capture->glass (GS)' samples taken there (_presentHeld) -- not
        // the next-rAF sample below.
        if (held) return;
        // Hidden tab: rAF is paused, so a callback queued now fires only when
        // the tab returns and would book a minutes-long "present" sample. No
        // glass while hidden -> no present / capture->glass sample.
        if (document.hidden) return;
        requestAnimationFrame((ts) => {
          const presentMs = performance.timeOrigin + ts;
          // F3: an rAF queued just before the tab went hidden fires only once
          // it's visible again -- not a real present sample.
          if (isStalePresentSample(t, presentMs)) return;
          this.segWindow.add('present', presentMs - t);
          if (rec && this.getMode() === 'gs') {
            const cap = capToGlass({ capToCompleteUs: rec.capUs, tEmitMs: rec.tEmitMs,
              tRecvMs: rec.tRecvMs, tPresentMs: presentMs });
            if (cap != null) this.segWindow.add('capture→glass (GS)', cap);
          }
        });
      },
      error: (e) => {
        console.error('[webgs] decoder error', e);
        // A key chunk was submitted and no output arrived since: this run
        // never produced a frame (unsupported codec, reclaimed hardware
        // decoder, ...) -- charge IdrRequester's backoff. An error after
        // output (the ordinary missing-reference case this feature exists
        // for) must not back off.
        if (this.keyPending) { this.idr.noteKeyFailed(); this.keyPending = false; }
        this.gate.onDecoderError();
        this.replaceDecoder();
        this.pollIdr();
      },
    });
  }

  // null when colortrans can't run (canvas not mounted yet, or failed).
  colorTrans() {
    if (this.ctFailed) return null;
    if (this.ct && !this.ct.ok) {
      console.error('[webgs] colortrans: WebGL context lost, video drawn flat from now on');
      this.ctFailed = true;
      return null;
    }
    if (!this.ct) {
      const c = this.getGlCanvas();
      if (!c) return null;
      try { this.ct = new ColorTransGl(c); }
      catch (e) {
        console.error('[webgs] colortrans unavailable, video drawn flat:', e);
        this.ctFailed = true;
        return null;
      }
    }
    return this.ct;
  }

  draw(frame) {
    const want = this.getColortrans();
    const ct = want ? this.colorTrans() : null;
    this.colour = ct ? 'colortrans' : want ? 'unavailable' : 'flat';
    let canvas;
    if (ct) {
      canvas = this.getGlCanvas();
      ct.draw(frame);
    } else {
      canvas = this.getCanvas();
      if (!canvas) return;
      if (canvas.width !== frame.displayWidth || canvas.height !== frame.displayHeight) {
        canvas.width = frame.displayWidth;
        canvas.height = frame.displayHeight;
      }
      canvas.getContext('2d').drawImage(frame, 0, 0);
    }
    // Only the canvas that got this frame is visible (the toggle flips live).
    if (canvas !== this.shown) {
      for (const c of [this.getCanvas(), this.getGlCanvas()]) if (c) c.style.visibility = c === canvas ? 'visible' : 'hidden';
      this.shown = canvas;
    }
  }

  onAuInner(buf, ptsUs, sid, flags, complete, tCompleteUs, hvccBuf, capUs, tEmitMs, tFirstUs) {
    const tRecvMs = now();
    this.segWindow.add('handoff', tRecvMs - tEmitMs);
    if (tFirstUs > 0 && tCompleteUs > 0) this.segWindow.add('fec', (tCompleteUs - tFirstUs) / 1000);
    this.metrics.addAu(tRecvMs, buf.byteLength);

    const data = new Uint8Array(buf);
    const g = this.gate.onAu({ sid, flags, complete: !!complete, data });
    if (g.reset) {
      this.replaceDecoder();
    }
    if (hvccBuf) this.hvcc = hvccBuf;
    if (!g.type) return;

    const pts = this.unwrap.add(ptsUs);
    this.period.add(pts);
    if (!this.decoder) {
      // A fresh decoder starts on a key frame: re-gate to the next IRAP.
      if (g.type !== 'key') { this.gate.onDecoderError(); return; }
      if (typeof VideoDecoder === 'undefined') {
        if (!this.noCodecWarned) { this.noCodecWarned = true; console.error('[webgs] WebCodecs VideoDecoder unavailable in this browser'); }
        this.gate.onDecoderError();
        return;
      }
      this.decoder = this.decoderSlot.replace(this.makeDecoder());
    }
    if (g.type === 'key' && (this.decoder.state !== 'configured' || this.configuredWith !== this.hvcc)) {
      if (!this.hvcc) {
        // hvcC hasn't arrived yet -- normally rides the very same AU (the
        // core builds it from the same VPS/SPS/PPS the gate just parsed),
        // so this is a stream-start race, not a permanent local failure:
        // another armed AU will very likely carry it. Charge the backoff
        // rather than either asking forever or suppressing for good.
        this.idr.noteKeyFailed();
        this.gate.onDecoderError();
        return;
      }
      this.decoder.configure({
        codec: 'hvc1.1.6.L120.B0', description: this.hvcc,
        hardwareAcceleration: 'prefer-hardware', optimizeForLatency: true,
      });
      this.configuredWith = this.hvcc;
    }
    const tSubmit = now();
    pruneSubmitted(this.submitted, tSubmit, 1000);   // dropped/never-output chunks
    this.submitted.set(pts, { tSubmit, tRecvMs, tEmitMs, capUs });
    if (g.type === 'key') this.keyPending = true;
    try {
      this.decoder.decode(new EncodedVideoChunk({ type: g.type, timestamp: pts, data: annexbToLengthPrefixed(data) }));
    } catch (e) {
      console.error('[webgs] decode() threw', e);
      if (this.keyPending) { this.idr.noteKeyFailed(); this.keyPending = false; }
      this.gate.onDecoderError();
      this.replaceDecoder();
    }
  }

  // Module.onAu argument order is web/src/web_main.cpp emit_au's.
  onAu(...a) {
    if (!this._phaseLive && this.getMode() === 'gs') {
      this._phaseLive = true;
      this._startPhaseLoop();
    }
    this.onAuInner(...a);
    this.pollIdr();
  }

  // GS-requested IDR: ask when the gate is unarmed, paced
  // by IdrRequester. Every early return in onAuInner still reaches this.
  // No VideoDecoder in this browser is a permanent, local, unfixable-by-a-
  // key-frame condition -- never ask for one.
  pollIdr() {
    if (typeof VideoDecoder === 'undefined') return;
    if (this.idr.poll(this.gate.armed, performance.now())) {
      // onWantIdr() returning exactly false means the request was swallowed
      // (e.g. session not live yet): don't count the poll, so the next AU
      // asks again instead of waiting out retryMs for nothing (finding 2).
      if (this.onWantIdr() === false) this.idr.cancel();
    }
  }

  // Session left live: release the hardware decoder now, not at the next Connect.
  close() {
    if (this._phaseLive) this._flushPhase();
    this.gate.onDecoderError();
    this.submitted.clear();
    this.decoder = this.decoderSlot.replace(null);
  }
}
