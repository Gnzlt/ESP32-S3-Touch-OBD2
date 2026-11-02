/* One ride's page, from window.RIDE (written by tools/build_site.py). */
"use strict";

(() => {
  const R = window.RIDE, m = R.meta, S = R.series, D = R.dist, X = R.derived || {};
  const { fmt, clock, token } = Charts;
  const app = document.getElementById("app");
  const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
  const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html !== undefined) e.innerHTML = html; return e; };
  const has = (v) => v !== null && v !== undefined;
  const C1 = token("--series-1"), C2 = token("--series-2");

  function when() {
    if (m.start) {
      return new Date(m.start).toLocaleString(undefined, { weekday: "short", day: "numeric", month: "short",
        year: "numeric", hour: "2-digit", minute: "2-digit" });
    }
    if (m.date) return new Date(m.date + "T12:00:00").toLocaleDateString(undefined, { weekday: "short", day: "numeric", month: "short", year: "numeric" });
    return "Date unknown (the board's clock wasn't set)";
  }

  function hm(s) {
    if (!has(s)) return "–";
    const h = Math.floor(s / 3600), mi = Math.round((s % 3600) / 60);
    return h ? `${h} h ${mi} min` : `${mi} min`;
  }

  // ---- header ------------------------------------------------------------------
  document.title = m.bike ? `Ride ${m.ride} · ${m.bike}` : `Ride ${m.ride}`;
  const head = el("header");
  head.appendChild(el("a", "back", "← All rides")).href = "index.html";
  const badges = m.in_progress ? '<span class="badge warn">last saved mid-ride</span>' : "";
  head.appendChild(el("h1", "", `Ride ${m.ride}${badges}`));
  head.appendChild(el("p", "sub", esc(when())));
  if (m.note) head.appendChild(el("p", "note", esc(m.note)));
  app.appendChild(head);

  // ---- tiles -------------------------------------------------------------------
  const k = m.fuel_k || 1;
  // Max lean, max G and min volts come from the rows (build_site.py): held for a
  // second, and after 10 s of running. The board's own figure shows when it
  // differs, as older firmware kept single samples (a jolt in a bag counted).
  const pick = (key) => (has(X[key]) ? X[key] : m[key]);
  const lean = (l, r) => `L ${fmt(l)}° · R ${fmt(r)}°`;
  const off = (key, by) => has(X[key]) && has(m[key]) && Math.abs(X[key] - m[key]) > by;
  const leanNote = off("max_lean_left_deg", 3) || off("max_lean_right_deg", 3)
    ? `held 1 s · board logged ${lean(m.max_lean_left_deg, m.max_lean_right_deg)}` : "held for a second";
  const tiles = [
    ["Distance", fmt(m.distance_km, 1), "km"],
    ["Duration", hm(m.duration_s), "", m.engine_s ? `engine ${hm(m.engine_s)}` : ""],
    ["Moving average", fmt(m.avg_kmh), "km/h"],
    ["Max speed", fmt(m.max_kmh), "km/h"],
    ["Max RPM", fmt(m.max_rpm), ""],
    ["Max coolant", fmt(m.max_coolant_c), "°C"],
    ["Min battery", fmt(pick("min_volts"), 2), "V",
      off("min_volts", 0.05) ? `engine running · board logged ${fmt(m.min_volts, 2)} V (cranking)` : "engine running"],
    ["Max lean", has(pick("max_lean_left_deg")) ? lean(pick("max_lean_left_deg"), pick("max_lean_right_deg")) : "–", "",
      has(X.max_lean_left_deg) ? leanNote : ""],
    ["Max G", fmt(pick("max_g"), 2), "g", off("max_g", 0.1) ? `held 1 s · board logged ${fmt(m.max_g, 2)} g` : ""],
    ["Fuel (est.)", has(m.fuel_raw_l) ? fmt(m.fuel_raw_l * k, 2) : "–", "L",
      has(m.fuel_raw_l100) ? `${fmt(m.fuel_raw_l100 * k, 1)} L/100 km` : ""],
    ["Alerts", fmt(m.alerts), ""],
    ["Fault codes", (m.fault_codes || []).length ? esc(m.fault_codes.join(" ")) : "none", ""],
  ];
  if (has(m.max_chip_c) || has(m.max_cell_c)) {
    tiles.push(["Hottest board", has(m.max_chip_c) ? fmt(m.max_chip_c) : "–", "°C",
      `chip${has(m.max_cell_c) ? ` · battery area ${fmt(m.max_cell_c)} °C` : ""}`]);
  }
  if (has(m.batt_end_pct)) {
    tiles.push(["Board battery", has(m.batt_start_pct) ? `${m.batt_start_pct}→${m.batt_end_pct}` : fmt(m.batt_end_pct), "%"]);
  }
  const tg = el("section", "tiles");
  for (const [key, v, unit, sub] of tiles) {
    tg.appendChild(el("div", "tile", `<div class="k">${key}</div><div class="v">${v}${unit ? `<small>${unit}</small>` : ""}</div>` +
      (sub ? `<div class="s">${esc(sub)}</div>` : "")));
  }
  app.appendChild(tg);

  // ---- over time -----------------------------------------------------------------
  const EVENT_TYPES = {
    alert: { label: "Alert", color: token("--critical") },
    code: { label: "Fault code", color: token("--serious") },
    obd: { label: "OBD link", color: token("--muted") },
    fillup: { label: "Fill-up", color: token("--good") },
    display: { label: "Display", color: token("--muted") },
    board: { label: "Board", color: token("--critical") },
  };
  const OBD_NAMES = { scan: "adapter lost, scanning", connect: "connecting", init: "initialising", ecu_off: "ECU quiet",
    live: "ECU answering", no_ble: "Bluetooth failed" };
  // Tyre readings get their own chart and card. A run of link retries (the
  // adapter switched off with the bike, or an ECU that won't answer) folds
  // into one row: a ride can end with dozens of scan/connect lines.
  const RETRIES = [["scan", "connect"], ["init", "ecu_off"]];
  const named = (e) => ({ ...e, detail: e.type === "obd" ? OBD_NAMES[e.detail] || e.detail : e.detail.replace(/_/g, " ") });
  const runs = [];
  for (const e of R.events) {
    if (e.type === "tyre") continue;
    const set = e.type === "obd" ? RETRIES.find((s) => s.includes(e.detail)) : null;
    const last = runs[runs.length - 1];
    if (set && last && last.set === set) last.items.push(e);
    else runs.push({ set, items: [e] });
  }
  const events = runs.flatMap(({ set, items }) => {
    if (!set || items.length < 4) return items.map(named);
    const what = set[0] === "scan" ? "adapter lost, kept searching" : "ECU not answering, kept trying";
    return [{ t: items[0].t, type: "obd", detail: `${what} (${items.length} steps until ${clock(items[items.length - 1].t)})` }];
  });

  const panel = (title, unit, keys, opts = {}) => {
    const series = keys.filter((key) => S[key]).map((key, i) => ({
      label: opts.labels ? opts.labels[keys.indexOf(key)] : R.columns[key][0],
      values: opts.scale ? S[key].map((v) => (has(v) ? +(v * opts.scale).toFixed(3) : v)) : S[key],
      color: [C1, C2][i],
    }));
    return series.length ? { title, unit, series, decimals: opts.decimals || 0, zero: opts.zero, symmetric: opts.symmetric, step: opts.step } : null;
  };
  const panels = [
    panel("Speed", "km/h", ["kmh"], { zero: true }),
    panel("RPM", "", ["rpm"], { zero: true }),
    panel("Gear", "", ["gear"], { step: true, zero: true }),
    panel("Throttle and load", "%", ["throttle_pct", "load_pct"], { zero: true, decimals: 1 }),
    panel("Intake pressure (MAP)", "kPa", ["map_kpa"]),
    panel("Temperatures", "°C", ["coolant_c", "iat_c"]),
    panel("Battery voltage", "V", ["volts"], { decimals: 2 }),
    panel("Fuel rate (est.)", "L/h", ["fuel_lph_raw"], { zero: true, decimals: 2, scale: k }),
    panel("Fuel used (est.)", "L", ["fuel_ml_raw"], { zero: true, decimals: 2, scale: k / 1000 }),
    panel("Ignition advance", "°", ["advance_deg"], { decimals: 1 }),
    panel("Short-term fuel trim", "%", ["stft_pct"], { symmetric: true, decimals: 1 }),
    panel("Lean", "° (− left, + right)", ["lean_deg"], { symmetric: true, decimals: 1 }),
    panel("G force", "g", ["lat_g", "lon_g"], { symmetric: true, decimals: 2, labels: ["Lateral", "Longitudinal"] }),
    panel("Tyre pressure", "kPa", ["front_kpa", "rear_kpa"], { labels: ["Front", "Rear"] }),
    panel("Tyre temperature", "°C", ["front_c", "rear_c"], { labels: ["Front", "Rear"] }),
    panel("Board temperature", "°C", ["chip_c", "cell_c"], { decimals: 1, labels: ["ESP32-S3 chip", "Battery area"] }),
    panel("Board battery", "%", ["board_soc"], { zero: true }),
    panel("Board current", "mA (− discharging)", ["board_ma"], { symmetric: true }),
    panel("Distance", "km", ["trip_km"], { zero: true, decimals: 2 }),
  ].filter(Boolean);

  app.appendChild(el("h2", "", "Over time"));
  app.appendChild(el("p", "hint", `${fmt(R.samples)} samples at 5 a second, drawn at one point a second ` +
    "(lean and G keep each second's peak). Time counts from the start of the ride." +
    (k !== 1 ? ` Fuel is the raw estimate × k ${fmt(k, 2)}.` : " Fuel is the uncalibrated estimate.")));
  // The toolbar sticks to the top only while the charts are on screen.
  const timeSection = el("section");
  app.appendChild(timeSection);
  const toolbar = el("div", "toolbar");
  timeSection.appendChild(toolbar);
  if (events.length) {
    const used = [...new Set(events.map((e) => e.type))];
    timeSection.appendChild(el("div", "events-key", "Markers: " + used.map((t) =>
      `<span><i style="background:${(EVENT_TYPES[t] || {}).color || token("--muted")}"></i>${(EVENT_TYPES[t] || { label: t }).label}</span>`).join("")));
  }
  const stack = el("div");
  timeSection.appendChild(stack);
  Charts.timeStack(stack, { t: S.t, panels, events, eventTypes: EVENT_TYPES, toolbar });

  // ---- distributions ---------------------------------------------------------------
  app.appendChild(el("h2", "", "Where the time went"));
  const grid = el("div", "grid2");
  app.appendChild(grid);
  const card = (title, sub) => {
    const c = el("div", "card");
    c.appendChild(el("h3", "", title));
    if (sub) c.appendChild(el("p", "hint", sub));
    grid.appendChild(c);
    return c;
  };
  const secs = (s) => (s >= 60 ? `${Math.floor(s / 60)} min ${Math.round(s % 60)} s` : `${fmt(s, 1)} s`);
  const histo = (title, sub, h, unit, color) => {
    if (!h) return;
    const labels = h.seconds.map((_, i) => fmt(h.lo + i * h.width));
    Charts.columns(card(title, sub), {
      labels, values: h.seconds, color, label: "none",
      tip: (i) => `<b>${fmt(h.lo + i * h.width)}–${fmt(h.lo + (i + 1) * h.width)} ${unit}</b><br>${secs(h.seconds[i])}`,
    });
  };
  if (D.gear) {
    const g = Object.keys(D.gear);
    Charts.columns(card("Time in each gear", "Seconds moving in each gear (estimated from rpm and speed)."), {
      labels: g.map((x) => `${x}`), values: g.map((x) => D.gear[x]), color: C1,
      tip: (i) => `<b>Gear ${g[i]}</b><br>${secs(D.gear[g[i]])}`,
    });
  }
  histo("RPM", "Seconds in each 500 rpm band, engine running.", D.rpm, "rpm", C1);
  histo("Speed", "Seconds in each 10 km/h band, moving.", D.kmh, "km/h", C1);
  histo("Lean angle", "Seconds in each 5° band, moving. Left is negative.", D.lean, "°", C1);
  if (D.fuel_sys) {
    const names = Object.keys(D.fuel_sys);
    Charts.columns(card("Fuel loop", "Seconds in each fuelling mode the ECU reported (PID 03)."), {
      labels: names.map((n) => n.replace("Open loop, ", "Open: ")), values: names.map((n) => D.fuel_sys[n]), color: C1,
      tip: (i) => `<b>${names[i]}</b><br>${secs(D.fuel_sys[names[i]])}`,
    });
  }
  if (D.gg) {
    Charts.scatter(card("G-G diagram", "Lateral against longitudinal G, every sample while riding."), {
      points: D.gg, color: C1, equal: true, height: 300, xLabel: "Lateral g (− left, + right)",
      yLabel: "Longitudinal g (+ accelerating)",
    });
  }

  // ---- events ------------------------------------------------------------------------
  app.appendChild(el("h2", "", "Events"));
  if (events.length) {
    const rows = events.map((e) => `<tr><td class="l">${clock(e.t)}</td><td class="l">${(EVENT_TYPES[e.type] || { label: e.type }).label}</td>` +
      `<td class="l">${esc(e.detail)}</td></tr>`).join("");
    app.appendChild(el("div", "table-wrap", `<table><thead><tr><th class="l">Time</th><th class="l">Type</th>` +
      `<th class="l">Detail</th></tr></thead><tbody>${rows}</tbody></table>`));
  } else {
    app.appendChild(el("p", "empty", "Nothing happened: no alerts, codes or link changes."));
  }

  // ---- details -----------------------------------------------------------------------
  app.appendChild(el("h2", "", "Details"));
  const dg = el("div", "grid2");
  app.appendChild(dg);
  const kv = (title, pairs) => {
    pairs = pairs.filter(([, v]) => has(v) && v !== "");
    if (!pairs.length) return;
    const c = el("div", "card");
    c.appendChild(el("h3", "", title));
    c.appendChild(el("dl", "kv", pairs.map(([a, b]) => `<dt>${a}</dt><dd>${esc(b)}</dd>`).join("")));
    dg.appendChild(c);
  };
  const ecu = m.ecu || {}, ad = m.adapter || {}, ln = m.link || {}, fu = m.fuel || {}, bb = m.board_battery || {};
  kv("Vehicle", [["VIN", ecu.vin], ["ECU software", (ecu.cal_ids || []).join(", ")], ["CVN", (ecu.cvns || []).join(", ")],
    ["Protocol", ecu.protocol], ["ECU address", ecu.address]]);
  kv("Adapter and link", [["Adapter", [ad.name, ad.elm].filter(Boolean).join(" · ")], ["Timeouts", ln.timeouts],
    ["Re-inits", ln.reinits], ["Link drops", ln.link_drops], ["ECU drops", ln.ecu_drops]]);
  kv("Fuel estimate", [["Raw litres", has(m.fuel_raw_l) ? fmt(m.fuel_raw_l, 3) : null], ["k", has(m.fuel_k) ? fmt(m.fuel_k, 3) : null],
    ["Model", fu.model], ["Coverage", has(fu.coverage_pct) ? `${fu.coverage_pct} %` : null],
    ["Since fill-up", has(fu.since_fill_km) ? `${fmt(fu.since_fill_km, 1)} km, ${fmt(fu.since_fill_raw_l, 2)} L raw` : null],
    ["Fill-ups marked", fu.fills]]);
  const heard = (name, t) => {
    const n = (R.tyres && R.tyres[name] || []).length;
    const parts = [];
    if (has(t.heard)) parts.push(`${fmt(t.heard)} packets${has(t.min_rssi_dbm) ? `, weakest ${t.min_rssi_dbm} dBm` : ""}`);
    if (n) parts.push(`${n} readings logged`);
    return parts.length ? ` (${parts.join(", ")})` : "";
  };
  // The range from the rows when there is one: it skips a sensor's wake-up packets.
  const tyres = m.tyres ? Object.entries(m.tyres).map(([name, t]) => [name, { ...t, ...((X.tyres || {})[name] || {}) }])
    .map(([name, t]) => [name[0].toUpperCase() + name.slice(1),
    !t.paired ? "not paired" : (has(t.min_kpa) ? `${fmt(t.min_kpa)}–${fmt(t.max_kpa)} kPa${has(t.max_c) ? `, max ${fmt(t.max_c)} °C` : ""}` : "not heard") +
      heard(name, t)])
    : [["Tyres", "–"]];
  kv("Tyres", tyres);
  kv("Board", [["Battery", bb.cell === false ? "no cell (USB power)" : has(m.batt_end_pct) ?
    `${has(m.batt_start_pct) ? m.batt_start_pct + " % → " : ""}${m.batt_end_pct} %${has(bb.end_mv) ? ` (${fmt(bb.end_mv / 1000, 2)} V)` : ""}` : null],
  ["Average current", has(bb.avg_ma) ? `${fmt(bb.avg_ma)} mA` : null],
  ["Gauge capacity", has(bb.full_mah) ? `${has(bb.remaining_mah) ? fmt(bb.remaining_mah) + " of " : ""}${fmt(bb.full_mah)} mAh` : null],
  ["Gyro offset", Array.isArray(m.gyro_bias_dps) ? `${m.gyro_bias_dps.map((v) => fmt(v, 2)).join(" / ")} °/s (x / y / z, learned at stops)` : null],
  ["Firmware", m.firmware], ["Ride uid", m.uid], ["Log format", m.format]]);

  app.appendChild(el("h2", "", "Files"));
  app.appendChild(el("ul", "files", R.files.map((f) => `<li><a href="${esc(f.href)}">${esc(f.name)}</a></li>`).join("")));
})();
