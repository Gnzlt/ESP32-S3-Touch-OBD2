/*
 * A small canvas chart library for the ride site, no dependencies:
 *   Charts.timeStack(el, cfg)  stacked time charts sharing one time axis: drag to zoom, double-click to
 *                              reset, a crosshair and readout across every chart, event markers
 *   Charts.columns(el, cfg)    columns (or dots, for values far from zero) with hover tooltips
 *   Charts.scatter(el, cfg)    an x/y scatter, optionally with a line through the origin
 * Colours come from the CSS tokens in style.css, read at each draw, so light and dark both work.
 */
"use strict";

const Charts = (() => {
  const DPR = () => window.devicePixelRatio || 1;
  const token = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  const FONT = '11px system-ui, -apple-system, "Segoe UI", sans-serif';
  const redrawers = new Set();

  matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => redrawers.forEach((f) => f()));

  // ---- helpers ----------------------------------------------------------------

  function fmt(v, decimals = 0) {
    if (v === null || v === undefined || Number.isNaN(v)) return "–";
    return Number(v).toLocaleString(undefined, { minimumFractionDigits: decimals, maximumFractionDigits: decimals });
  }

  function clock(s) {
    s = Math.max(0, Math.round(s));
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
    const mm = String(m).padStart(h ? 2 : 1, "0"), ss = String(sec).padStart(2, "0");
    return h ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
  }

  // Round tick values covering lo..hi, about n of them.
  function ticks(lo, hi, n = 4) {
    if (!(hi > lo)) { hi = lo + 1; }
    const raw = (hi - lo) / n;
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const step = [1, 2, 2.5, 5, 10].map((f) => f * mag).find((s) => s >= raw) || 10 * mag;
    const out = [];
    for (let v = Math.ceil(lo / step - 1e-9) * step; v <= hi + 1e-9; v += step) out.push(+v.toFixed(10));
    return { values: out, step };
  }

  // Ticks on a clock's steps (10 s, 5 min, 1 h...), about n of them.
  const CLOCK_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600];
  function timeTicks(lo, hi, n = 4) {
    if (!(hi > lo)) { hi = lo + 1; }
    const raw = (hi - lo) / n;
    const step = CLOCK_STEPS.find((s) => s >= raw) || CLOCK_STEPS[CLOCK_STEPS.length - 1];
    const out = [];
    for (let v = Math.ceil(lo / step - 1e-9) * step; v <= hi + 1e-9; v += step) out.push(v);
    return { values: out, step };
  }

  function compact(v) {
    const a = Math.abs(v);
    if (a >= 1000) return (v / 1000).toLocaleString(undefined, { maximumFractionDigits: 1 }) + "K";
    return v.toLocaleString(undefined, { maximumFractionDigits: 2 });
  }

  // A canvas filling el's width at a CSS height (a number, or a function of
  // the window), sharp on HiDPI screens. fit() sizes it and returns the context.
  function makeCanvas(el, height) {
    const cv = document.createElement("canvas");
    el.appendChild(cv);
    const fit = () => {
      const w = Math.max(10, el.clientWidth);
      const h = typeof height === "function" ? height() : height;
      cv.width = Math.round(w * DPR());
      cv.height = Math.round(h * DPR());
      cv.style.height = h + "px";
      const ctx = cv.getContext("2d");
      ctx.setTransform(DPR(), 0, 0, DPR(), 0, 0);
      return { ctx, w, h };
    };
    return { cv, fit };
  }

  let tip;
  function tooltip(html, x, y) {
    if (!tip) {
      tip = document.createElement("div");
      tip.className = "tooltip";
      document.body.appendChild(tip);
    }
    if (html === null) { tip.style.display = "none"; return; }
    tip.innerHTML = html;
    tip.style.display = "block";
    const r = tip.getBoundingClientRect();
    let left = x + 14, top = y + 14;
    if (left + r.width > innerWidth - 8) left = x - r.width - 14;
    if (top + r.height > innerHeight - 8) top = y - r.height - 14;
    tip.style.left = Math.max(8, left) + "px";
    tip.style.top = Math.max(8, top) + "px";
  }

  function onResize(el, draw) {
    redrawers.add(draw);
    let w = el.clientWidth;
    new ResizeObserver(() => { if (el.clientWidth !== w) { w = el.clientWidth; draw(); } }).observe(el);
  }

  function roundTop(ctx, x, y, w, h, r) {
    // A column: rounded at its data end, square at the baseline. h < 0 grows downward.
    r = Math.min(r, Math.abs(h), w / 2);
    ctx.beginPath();
    if (h >= 0) {
      ctx.moveTo(x, y + h);
      ctx.lineTo(x, y + r);
      ctx.arcTo(x, y, x + r, y, r);
      ctx.lineTo(x + w - r, y);
      ctx.arcTo(x + w, y, x + w, y + r, r);
      ctx.lineTo(x + w, y + h);
    } else {
      ctx.moveTo(x, y + h);
      ctx.lineTo(x, y - r);
      ctx.arcTo(x, y, x + r, y, r);
      ctx.lineTo(x + w - r, y);
      ctx.arcTo(x + w, y, x + w, y - r, r);
      ctx.lineTo(x + w, y + h);
    }
    ctx.closePath();
    ctx.fill();
  }

  function dot(ctx, x, y, r, color) {
    ctx.beginPath();
    ctx.arc(x, y, r + 2, 0, 2 * Math.PI);
    ctx.fillStyle = token("--surface");
    ctx.fill();
    ctx.beginPath();
    ctx.arc(x, y, r, 0, 2 * Math.PI);
    ctx.fillStyle = color;
    ctx.fill();
  }

  // Index of the last t <= x (binary search).
  function lastAtOrBefore(t, x) {
    let lo = 0, hi = t.length - 1;
    if (x < t[0]) return -1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (t[mid] <= x) lo = mid; else hi = mid - 1;
    }
    return lo;
  }

  // ---- stacked time charts ----------------------------------------------------

  /*
   * cfg = {t: [s...], panels: [{title, unit, decimals, series: [{label, values, color}], step, zero,
   *        symmetric}], events: [{t, type, detail}], eventTypes: {type: {label, color}},
   *        toolbar: element for the time readout and reset button}
   */
  function timeStack(root, cfg) {
    const t = cfg.t;
    if (!t.length) { root.innerHTML = '<p class="empty">No samples in this ride.</p>'; return; }
    const full = [t[0], t[t.length - 1] + 1];
    let view = full.slice();
    let hover = null;        // time under the pointer
    let drag = null;         // {panel, x0, x1} in CSS px
    const M = { l: 46, r: 12, t: 8, b: 20 };
    const height = () => (innerWidth < 600 ? 96 : 120);

    const bar = cfg.toolbar;
    bar.innerHTML = '<span class="time">–</span><span class="help">Drag across a chart to zoom in. ' +
      'Double-click or Reset to see the whole ride.</span><button type="button" disabled>Reset zoom</button>';
    const timeEl = bar.querySelector(".time"), resetBtn = bar.querySelector("button");
    resetBtn.addEventListener("click", () => setView(full));

    const panels = cfg.panels.map((p) => {
      const el = document.createElement("div");
      el.className = "panel";
      const head = document.createElement("div");
      head.className = "panel-head";
      head.innerHTML = `<span class="title">${p.title}<span class="unit">${p.unit || ""}</span></span>`;
      const readouts = p.series.map((s) => {
        const lg = document.createElement("span");
        lg.className = "legend";
        // One series: the title names it, so no swatch.
        lg.innerHTML = (p.series.length > 1 ? `<i style="background:${s.color}"></i>${s.label} ` : "") + "<b>–</b>";
        head.appendChild(lg);
        return lg.querySelector("b");
      });
      el.appendChild(head);
      const box = document.createElement("div");
      box.className = "chart";
      el.appendChild(box);
      root.appendChild(el);
      const c = makeCanvas(box, height);
      return { cfg: p, el, box, c, readouts, geo: null };
    });

    function yRange(p) {
      let lo = Infinity, hi = -Infinity;
      const i0 = Math.max(0, lastAtOrBefore(t, view[0])), i1 = lastAtOrBefore(t, view[1]);
      for (const s of p.series) {
        for (let i = i0; i <= i1; i++) {
          const v = s.values[i];
          if (v === null || v === undefined) continue;
          if (v < lo) lo = v;
          if (v > hi) hi = v;
        }
      }
      if (lo === Infinity) return null;
      if (p.zero) { lo = Math.min(0, lo); hi = Math.max(0, hi); }
      if (p.symmetric) { const a = Math.max(Math.abs(lo), Math.abs(hi), 1e-6); lo = -a; hi = a; }
      if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
      const tk = ticks(lo, hi, 3);
      return { lo: Math.min(lo, tk.values[0]), hi: Math.max(hi, tk.values[tk.values.length - 1]), ticks: tk.values };
    }

    function draw(pn) {
      const { ctx, w, h } = pn.c.fit();
      const p = pn.cfg;
      ctx.clearRect(0, 0, w, h);
      const pw = w - M.l - M.r, ph = h - M.t - M.b;
      const X = (s) => M.l + ((s - view[0]) / (view[1] - view[0])) * pw;
      const yr = yRange(p);
      pn.geo = { X, pw, ph, w, h };
      ctx.font = FONT;
      // Time ticks along the bottom of every chart.
      const xt = timeTicks(view[0], view[1], Math.max(2, Math.floor(pw / 90)));
      ctx.fillStyle = token("--muted");
      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      ctx.strokeStyle = token("--grid");
      ctx.lineWidth = 1;
      for (const v of xt.values) {
        const x = Math.round(X(v)) + 0.5;
        ctx.beginPath(); ctx.moveTo(x, M.t); ctx.lineTo(x, M.t + ph); ctx.stroke();
        ctx.fillText(clock(v), x, M.t + ph + 5);
      }
      if (!yr) {
        ctx.fillStyle = token("--muted");
        ctx.textAlign = "left";
        ctx.textBaseline = "middle";
        ctx.fillText("no data in this range", M.l + 8, M.t + ph / 2);
        return;
      }
      const Y = (v) => M.t + ph - ((v - yr.lo) / (yr.hi - yr.lo)) * ph;
      pn.geo.Y = Y;
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      for (const v of yr.ticks) {
        const y = Math.round(Y(v)) + 0.5;
        ctx.strokeStyle = v === 0 ? token("--axis") : token("--grid");
        ctx.beginPath(); ctx.moveTo(M.l, y); ctx.lineTo(M.l + pw, y); ctx.stroke();
        ctx.fillStyle = token("--muted");
        ctx.fillText(compact(v), M.l - 6, y);
      }
      // Events: a hairline in the event's colour, a ringed dot at the top.
      for (const e of cfg.events || []) {
        if (e.t < view[0] || e.t > view[1]) continue;
        const x = Math.round(X(e.t)) + 0.5, col = (cfg.eventTypes[e.type] || {}).color || token("--muted");
        ctx.globalAlpha = 0.55;
        ctx.strokeStyle = col;
        ctx.beginPath(); ctx.moveTo(x, M.t); ctx.lineTo(x, M.t + ph); ctx.stroke();
        ctx.globalAlpha = 1;
      }
      // The series: 2px lines, broken where data is missing or a gap is over 2 s.
      ctx.save();
      ctx.beginPath(); ctx.rect(M.l, M.t - 2, pw, ph + 4); ctx.clip();
      ctx.lineWidth = 2;
      ctx.lineJoin = ctx.lineCap = "round";
      const i0 = Math.max(0, lastAtOrBefore(t, view[0]) - 1), i1 = Math.min(t.length - 1, lastAtOrBefore(t, view[1]) + 1);
      for (const s of p.series) {
        ctx.strokeStyle = s.color;
        ctx.beginPath();
        let open = false, prevT = null, prevY = null;
        for (let i = i0; i <= i1; i++) {
          const v = s.values[i];
          if (v === null || v === undefined || (prevT !== null && t[i] - prevT > 2)) {
            open = false;
            if (v === null || v === undefined) { prevT = null; continue; }
          }
          const x = X(t[i]), y = Y(v);
          if (!open) { ctx.moveTo(x, y); open = true; }
          else if (p.step) { ctx.lineTo(x, prevY); ctx.lineTo(x, y); }
          else ctx.lineTo(x, y);
          prevT = t[i]; prevY = y;
        }
        ctx.stroke();
      }
      ctx.restore();
      for (const e of cfg.events || []) {
        if (e.t < view[0] || e.t > view[1]) continue;
        dot(ctx, X(e.t), M.t + 3, 3, (cfg.eventTypes[e.type] || {}).color || token("--muted"));
      }
      // The zoom being dragged out.
      if (drag && drag.panel === pn) {
        const a = Math.min(drag.x0, drag.x1), b = Math.max(drag.x0, drag.x1);
        ctx.fillStyle = token("--wash");
        ctx.fillRect(a, M.t, b - a, ph);
        ctx.strokeStyle = token("--axis");
        ctx.strokeRect(a + 0.5, M.t + 0.5, b - a, ph);
      }
      // The crosshair and each series' value at it.
      if (hover !== null && hover >= view[0] && hover <= view[1]) {
        const i = lastAtOrBefore(t, hover);
        const x = Math.round(X(hover)) + 0.5;
        ctx.strokeStyle = token("--ink-2");
        ctx.beginPath(); ctx.moveTo(x, M.t); ctx.lineTo(x, M.t + ph); ctx.stroke();
        p.series.forEach((s) => {
          const v = i >= 0 && hover - t[i] <= 2 ? s.values[i] : null;
          if (v !== null && v !== undefined) dot(ctx, x, Y(v), 4, s.color);
        });
      }
    }

    function updateReadouts() {
      const i = hover === null ? -1 : lastAtOrBefore(t, hover);
      timeEl.textContent = hover === null ? "–" : clock(hover);
      for (const pn of panels) {
        pn.cfg.series.forEach((s, k) => {
          const v = i >= 0 && hover - t[i] <= 2 ? s.values[i] : null;
          pn.readouts[k].textContent = fmt(v, pn.cfg.decimals || 0);
        });
      }
    }

    let queued = false;
    function drawAll() {
      if (queued) return;
      queued = true;
      requestAnimationFrame(() => { queued = false; panels.forEach(draw); updateReadouts(); });
    }

    function setView(v) {
      view = [Math.max(full[0], v[0]), Math.min(full[1], v[1])];
      if (view[1] - view[0] < 5) view = [view[0], view[0] + 5];
      resetBtn.disabled = view[0] === full[0] && view[1] === full[1];
      drawAll();
    }

    const timeAt = (pn, x) => view[0] + ((x - M.l) / pn.geo.pw) * (view[1] - view[0]);

    for (const pn of panels) {
      const cv = pn.c.cv;
      const local = (ev) => { const r = cv.getBoundingClientRect(); return [ev.clientX - r.left, ev.clientY - r.top]; };
      cv.addEventListener("pointerdown", (ev) => {
        if (ev.pointerType === "touch") return;
        const [x] = local(ev);
        drag = { panel: pn, x0: x, x1: x };
        cv.setPointerCapture(ev.pointerId);
      });
      cv.addEventListener("pointermove", (ev) => {
        const [x, y] = local(ev);
        if (!pn.geo) return;
        hover = Math.min(view[1], Math.max(view[0], timeAt(pn, x)));
        if (drag) drag.x1 = Math.min(M.l + pn.geo.pw, Math.max(M.l, x));
        // An event under the pointer: say what it was.
        const near = (cfg.events || []).find((e) => e.t >= view[0] && e.t <= view[1] && Math.abs(pn.geo.X(e.t) - x) < 5);
        if (near && !drag) {
          const et = cfg.eventTypes[near.type] || { label: near.type };
          tooltip(`<b>${et.label}</b> ${near.detail ? "· " + near.detail : ""}<br><span class="t-muted">${clock(near.t)}</span>`,
            ev.clientX, ev.clientY);
        } else {
          tooltip(null);
        }
        drawAll();
      });
      cv.addEventListener("pointerup", () => {
        if (drag && Math.abs(drag.x1 - drag.x0) > 6) {
          const a = timeAt(pn, Math.min(drag.x0, drag.x1)), b = timeAt(pn, Math.max(drag.x0, drag.x1));
          drag = null;
          setView([a, b]);
        } else {
          drag = null;
          drawAll();
        }
      });
      cv.addEventListener("pointerleave", () => { if (!drag) { hover = null; tooltip(null); drawAll(); } });
      cv.addEventListener("dblclick", () => setView(full));
    }
    onResize(root, drawAll);
    drawAll();
  }

  // ---- columns ------------------------------------------------------------------

  /*
   * cfg = {labels: [...], values: [...], color, height, decimals, unit, dots (plot dots on a range that
   *        needn't include zero), tip: (i) => html, click: (i) => void, label: "max" | "none"}
   */
  function columns(root, cfg) {
    const box = document.createElement("div");
    box.className = "chart";
    root.appendChild(box);
    const height = cfg.height || 180;
    const c = makeCanvas(box, height);
    const vals = cfg.values;
    if (!vals.some((v) => v !== null && v !== undefined)) {
      box.innerHTML = '<p class="empty">No data.</p>';
      return;
    }
    let hoverI = -1, geo = null;
    const M = { l: 46, r: 10, t: 16, b: 22 };

    function draw() {
      const { ctx, w, h } = c.fit();
      ctx.clearRect(0, 0, w, h);
      ctx.font = FONT;
      const pw = w - M.l - M.r, ph = h - M.t - M.b;
      const present = vals.filter((v) => v !== null && v !== undefined);
      let lo = Math.min(...present), hi = Math.max(...present);
      if (!cfg.dots) { lo = Math.min(0, lo); hi = Math.max(0, hi); }
      else { const pad = (hi - lo) * 0.15 || 1; lo -= pad; hi += pad; }
      const tk = ticks(lo, hi, 4);
      lo = Math.min(lo, tk.values[0]);
      hi = Math.max(hi, tk.values[tk.values.length - 1]);
      const Y = (v) => M.t + ph - ((v - lo) / (hi - lo)) * ph;
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      for (const v of tk.values) {
        const y = Math.round(Y(v)) + 0.5;
        ctx.strokeStyle = v === 0 && !cfg.dots ? token("--axis") : token("--grid");
        ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(M.l, y); ctx.lineTo(M.l + pw, y); ctx.stroke();
        ctx.fillStyle = token("--muted");
        ctx.fillText(compact(v), M.l - 6, y);
      }
      const n = vals.length, slot = pw / n;
      const bw = Math.max(2, Math.min(24, slot - 2));
      geo = { slot, n };
      // Bottom labels: as many as fit.
      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      ctx.fillStyle = token("--muted");
      const every = Math.max(1, Math.ceil(n / Math.max(1, Math.floor(pw / 44))));
      cfg.labels.forEach((l, i) => {
        if (i % every === 0) ctx.fillText(l, M.l + slot * (i + 0.5), M.t + ph + 6);
      });
      let maxI = -1;
      vals.forEach((v, i) => { if (v !== null && v !== undefined && (maxI < 0 || v > vals[maxI])) maxI = i; });
      vals.forEach((v, i) => {
        if (v === null || v === undefined) return;
        const x = M.l + slot * (i + 0.5);
        ctx.globalAlpha = hoverI >= 0 && hoverI !== i ? 0.55 : 1;
        if (cfg.dots) {
          dot(ctx, x, Y(v), 4, cfg.color);
        } else {
          ctx.fillStyle = cfg.color;
          const y0 = Y(0), y1 = Y(v);
          roundTop(ctx, x - bw / 2, Math.min(y0, y1), bw, Math.abs(y0 - y1), 4);
        }
        ctx.globalAlpha = 1;
      });
      if (maxI >= 0 && cfg.label !== "none") {
        ctx.fillStyle = token("--ink-2");
        ctx.textAlign = "center";
        ctx.textBaseline = "bottom";
        const x = M.l + slot * (maxI + 0.5);
        ctx.fillText(fmt(vals[maxI], cfg.decimals || 0), x, Y(vals[maxI]) - (cfg.dots ? 8 : 3));
      }
    }

    c.cv.addEventListener("pointermove", (ev) => {
      if (!geo) return;
      const r = c.cv.getBoundingClientRect();
      const i = Math.floor((ev.clientX - r.left - M.l) / geo.slot);
      const ok = i >= 0 && i < geo.n && vals[i] !== null && vals[i] !== undefined;
      const ni = ok ? i : -1;
      c.cv.style.cursor = ok && cfg.click ? "pointer" : "default";
      if (ni !== hoverI) { hoverI = ni; draw(); }
      if (ok) {
        tooltip(cfg.tip ? cfg.tip(i) : `<b>${cfg.labels[i]}</b><br>${fmt(vals[i], cfg.decimals || 0)} ${cfg.unit || ""}`,
          ev.clientX, ev.clientY);
      } else {
        tooltip(null);
      }
    });
    c.cv.addEventListener("pointerleave", () => { hoverI = -1; tooltip(null); draw(); });
    c.cv.addEventListener("click", () => { if (hoverI >= 0 && cfg.click) cfg.click(hoverI); });
    onResize(box, draw);
    draw();
  }

  // ---- scatter ------------------------------------------------------------------

  /*
   * cfg = {points: [[x, y], ...], color, height, xLabel, yLabel, equal (same scale both ways, centred
   *        on 0), r (dot radius), alpha, slope (a line y = slope * x), tip: (i) => html}
   */
  function scatter(root, cfg) {
    const box = document.createElement("div");
    box.className = "chart";
    root.appendChild(box);
    const height = cfg.height || 260;
    const c = makeCanvas(box, height);
    const pts = cfg.points;
    if (!pts.length) { box.innerHTML = '<p class="empty">No data.</p>'; return; }
    let geo = null, hoverI = -1;
    const M = { l: 46, r: 12, t: 10, b: 34 };

    function draw() {
      const { ctx, w, h } = c.fit();
      ctx.clearRect(0, 0, w, h);
      ctx.font = FONT;
      let pw = w - M.l - M.r, ph = h - M.t - M.b;
      let xlo, xhi, ylo, yhi;
      if (cfg.equal) {
        const a = Math.max(0.5, ...pts.map((p) => Math.max(Math.abs(p[0]), Math.abs(p[1])))) * 1.1;
        xlo = ylo = -a; xhi = yhi = a;
        const side = Math.min(pw, ph);
        M.l = (w - side) / 2 + 20; pw = ph = side - 20;
      } else {
        xlo = 0; ylo = 0;
        xhi = Math.max(...pts.map((p) => p[0])) * 1.15 || 1;
        yhi = Math.max(...pts.map((p) => p[1])) * 1.15 || 1;
      }
      const xt = ticks(xlo, xhi, 4), yt = ticks(ylo, yhi, 4);
      const X = (v) => M.l + ((v - xlo) / (xhi - xlo)) * pw, Y = (v) => M.t + ph - ((v - ylo) / (yhi - ylo)) * ph;
      geo = { X, Y };
      ctx.lineWidth = 1;
      ctx.fillStyle = token("--muted");
      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      for (const v of xt.values) {
        if (v < xlo || v > xhi) continue;
        const x = Math.round(X(v)) + 0.5;
        ctx.strokeStyle = v === 0 ? token("--axis") : token("--grid");
        ctx.beginPath(); ctx.moveTo(x, M.t); ctx.lineTo(x, M.t + ph); ctx.stroke();
        ctx.fillText(compact(v), x, M.t + ph + 5);
      }
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      for (const v of yt.values) {
        if (v < ylo || v > yhi) continue;
        const y = Math.round(Y(v)) + 0.5;
        ctx.strokeStyle = v === 0 ? token("--axis") : token("--grid");
        ctx.beginPath(); ctx.moveTo(M.l, y); ctx.lineTo(M.l + pw, y); ctx.stroke();
        ctx.fillText(compact(v), M.l - 6, y);
      }
      ctx.textAlign = "center";
      ctx.textBaseline = "bottom";
      ctx.fillStyle = token("--ink-2");
      ctx.fillText(cfg.xLabel || "", M.l + pw / 2, h - 2);
      ctx.save();
      ctx.translate(11, M.t + ph / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.textBaseline = "middle";
      ctx.fillText(cfg.yLabel || "", 0, 0);
      ctx.restore();
      if (cfg.slope) {
        ctx.strokeStyle = token("--ink-2");
        ctx.setLineDash([]);
        ctx.lineWidth = 1;
        ctx.beginPath();
        const xe = Math.min(xhi, yhi / cfg.slope);
        ctx.moveTo(X(0), Y(0)); ctx.lineTo(X(xe), Y(cfg.slope * xe));
        ctx.stroke();
      }
      const r = cfg.r || 2.5;
      if (r < 4) {
        ctx.globalAlpha = cfg.alpha || 0.35;
        ctx.fillStyle = cfg.color;
        for (const p of pts) { ctx.beginPath(); ctx.arc(X(p[0]), Y(p[1]), r, 0, 2 * Math.PI); ctx.fill(); }
        ctx.globalAlpha = 1;
      } else {
        pts.forEach((p, i) => dot(ctx, X(p[0]), Y(p[1]), i === hoverI ? r + 1 : r, cfg.color));
      }
    }

    if (cfg.tip) {
      c.cv.addEventListener("pointermove", (ev) => {
        if (!geo) return;
        const rc = c.cv.getBoundingClientRect(), x = ev.clientX - rc.left, y = ev.clientY - rc.top;
        let best = -1, bd = 12 * 12;
        pts.forEach((p, i) => { const d = (geo.X(p[0]) - x) ** 2 + (geo.Y(p[1]) - y) ** 2; if (d < bd) { bd = d; best = i; } });
        if (best !== hoverI) { hoverI = best; draw(); }
        tooltip(best >= 0 ? cfg.tip(best) : null, ev.clientX, ev.clientY);
      });
      c.cv.addEventListener("pointerleave", () => { hoverI = -1; tooltip(null); draw(); });
    }
    onResize(box, draw);
    draw();
  }

  return { timeStack, columns, scatter, fmt, clock, token };
})();
