/* The overview page, from window.SITE (written by tools/build_site.py). */
"use strict";

(() => {
  const { rides, fillups, bike } = window.SITE;
  const { fmt, token } = Charts;
  const app = document.getElementById("app");
  const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
  const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html !== undefined) e.innerHTML = html; return e; };
  const has = (v) => v !== null && v !== undefined;
  const sum = (xs) => xs.reduce((a, b) => a + b, 0);
  const C1 = token("--series-1");

  const dateOf = (r) => (r.start ? new Date(r.start) : r.date ? new Date(r.date + "T12:00:00") : null);
  const dateText = (r) => {
    const d = dateOf(r);
    if (!d) return "–";
    return r.start ? d.toLocaleString(undefined, { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" })
      : d.toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" });
  };
  const hm = (s) => {
    if (!has(s)) return "–";
    const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60);
    return h ? `${h} h ${String(m).padStart(2, "0")}` : `${m} min`;
  };

  // ---- header and totals --------------------------------------------------------
  const dated = rides.map(dateOf).filter(Boolean).sort((a, b) => a - b);
  app.appendChild(el("h1", "", bike ? `${bike} rides` : "Rides"));
  app.appendChild(el("p", "sub", rides.length ? `${rides.length} ride${rides.length > 1 ? "s" : ""}` +
    (dated.length ? ` · ${dated[0].toLocaleDateString()} to ${dated[dated.length - 1].toLocaleDateString()}` : "") :
    "No rides yet. Pull them from the board with tools/pull_logs.py."));
  const km = sum(rides.map((r) => r.distance_km || 0));
  const fuelRides = rides.filter((r) => has(r.fuel_l) && (r.distance_km || 0) >= 1);
  const fuelKm = sum(fuelRides.map((r) => r.distance_km)), fuelL = sum(fuelRides.map((r) => r.fuel_l));
  const tiles = [
    ["Rides", fmt(rides.length), ""],
    ["Distance", fmt(km, km < 100 ? 1 : 0), "km"],
    ["Riding time", hm(sum(rides.map((r) => r.duration_s || 0))), ""],
    ["Engine time", hm(sum(rides.map((r) => r.engine_s || 0))), ""],
    ["Fuel economy (est.)", fuelKm ? fmt((fuelL / fuelKm) * 100, 1) : "–", "L/100 km", fuelKm ? `over ${fmt(fuelKm, 0)} km` : "no fuel data yet"],
    ["Top speed", fmt(Math.max(0, ...rides.map((r) => r.max_kmh || 0))), "km/h"],
    ["Max lean", fmt(Math.max(0, ...rides.map((r) => r.max_lean_deg || 0))), "°"],
    ["Alerts", fmt(sum(rides.map((r) => r.alerts || 0))), ""],
  ];
  const tg = el("section", "tiles");
  for (const [k, v, unit, sub] of tiles) {
    tg.appendChild(el("div", "tile", `<div class="k">${k}</div><div class="v">${v}${unit ? `<small>${unit}</small>` : ""}</div>` +
      (sub ? `<div class="s">${esc(sub)}</div>` : "")));
  }
  app.appendChild(tg);

  // ---- per-ride charts ----------------------------------------------------------
  if (rides.length) {
    app.appendChild(el("h2", "", "Ride by ride"));
    const grid = el("div", "grid2");
    app.appendChild(grid);
    const labels = rides.map((r) => `${r.ride}`);
    const open = (i) => { location.href = rides[i].href; };
    const tipFor = (title, v) => (i) => `<b>Ride ${rides[i].ride}</b> <span class="t-muted">${esc(dateText(rides[i]))}</span><br>${title}: ${v(rides[i])}`;
    const card = (title, sub) => {
      const c = el("div", "card");
      c.appendChild(el("h3", "", title));
      c.appendChild(el("p", "hint", sub));
      grid.appendChild(c);
      return c;
    };
    Charts.columns(card("Distance", "km per ride. Click a column to open the ride."), {
      labels, values: rides.map((r) => r.distance_km), color: C1, decimals: 1, click: open,
      tip: tipFor("Distance", (r) => `${fmt(r.distance_km, 1)} km`),
    });
    Charts.columns(card("Fuel economy (est.)", "L/100 km, rides of 1 km or more."), {
      labels, values: rides.map((r) => ((r.distance_km || 0) >= 1 ? r.l100 : null)), color: C1, decimals: 1, click: open,
      tip: tipFor("Fuel", (r) => `${fmt(r.l100, 1)} L/100 km`),
    });
    Charts.columns(card("Max coolant", "°C, the hottest the engine got on each ride."), {
      labels, values: rides.map((r) => r.max_coolant_c), color: C1, dots: true, click: open,
      tip: tipFor("Max coolant", (r) => `${fmt(r.max_coolant_c)} °C`),
    });
    if (rides.some((r) => has(r.max_chip_c))) {
      Charts.columns(card("Hottest board", "°C, the ESP32-S3 chip's highest reading on each ride (warning at 80)."), {
        labels, values: rides.map((r) => r.max_chip_c), color: C1, dots: true, click: open,
        tip: tipFor("Hottest chip", (r) => `${fmt(r.max_chip_c, 1)} °C` +
          (has(r.max_cell_c) ? `<br>Battery area: ${fmt(r.max_cell_c, 1)} °C` : "")),
      });
    }
    Charts.columns(card("Min battery voltage", "V, the lowest with the engine running."), {
      labels, values: rides.map((r) => r.min_volts), color: C1, dots: true, decimals: 2, click: open,
      tip: tipFor("Min voltage", (r) => `${fmt(r.min_volts, 2)} V`),
    });
  }

  // ---- the rides table ------------------------------------------------------------
  app.appendChild(el("h2", "", "All rides"));
  const cols = [
    ["ride", "Ride", (r) => `${r.ride}${r.in_progress ? " ▲" : ""}`, "l"],
    ["date", "Date", (r) => esc(dateText(r)), "l", (r) => (dateOf(r) || 0) - 0],
    ["duration_s", "Time", (r) => hm(r.duration_s)],
    ["distance_km", "km", (r) => fmt(r.distance_km, 1)],
    ["avg_kmh", "Avg km/h", (r) => fmt(r.avg_kmh)],
    ["max_kmh", "Max km/h", (r) => fmt(r.max_kmh)],
    ["max_rpm", "Max rpm", (r) => fmt(r.max_rpm)],
    ["max_lean_deg", "Max lean", (r) => (has(r.max_lean_deg) ? fmt(r.max_lean_deg) + "°" : "–")],
    ["l100", "L/100 km", (r) => ((r.distance_km || 0) >= 1 ? fmt(r.l100, 1) : "–")],
    ["alerts", "Alerts", (r) => fmt(r.alerts)],
    ["codes", "Codes", (r) => esc(r.codes.join(" ") || "–"), "l", (r) => r.codes.length],
  ];
  const wrap = el("div", "table-wrap");
  const table = el("table");
  table.innerHTML = `<thead><tr>${cols.map(([k, h, , cls]) => `<th class="sort ${cls || ""}" data-k="${k}">${h}</th>`).join("")}</tr></thead><tbody></tbody>`;
  wrap.appendChild(table);
  app.appendChild(wrap);
  if (rides.some((r) => r.in_progress)) app.appendChild(el("p", "hint", "▲ last saved mid-ride: the board lost power before the ride's end was written."));
  let sortKey = "ride", dir = -1;
  function renderRows() {
    const col = cols.find((c) => c[0] === sortKey);
    const key = col[4] || ((r) => r[sortKey]);
    const sorted = rides.slice().sort((a, b) => {
      const x = key(a), y = key(b);
      if (!has(x) && !has(y)) return 0;
      if (!has(x)) return 1;
      if (!has(y)) return -1;
      return (x < y ? -1 : x > y ? 1 : 0) * dir;
    });
    table.tBodies[0].innerHTML = sorted.map((r) => `<tr class="link" data-href="${esc(r.href)}">` + cols.map(([k, , f, cls]) =>
      `<td class="${cls || ""} ${k === "codes" && r.codes.length ? "codes" : ""}">${k === "ride" ? `<a href="${esc(r.href)}">${f(r)}</a>` : f(r)}</td>`).join("") +
      "</tr>").join("");
    table.querySelectorAll("th").forEach((th) => th.setAttribute("aria-sort", th.dataset.k === sortKey ? (dir > 0 ? "ascending" : "descending") : "none"));
  }
  table.tHead.addEventListener("click", (ev) => {
    const k = ev.target.dataset && ev.target.dataset.k;
    if (!k) return;
    if (k === sortKey) dir = -dir; else { sortKey = k; dir = k === "ride" || k === "date" ? -1 : -1; }
    renderRows();
  });
  table.tBodies[0].addEventListener("click", (ev) => {
    const tr = ev.target.closest("tr.link");
    if (tr && !ev.target.closest("a")) location.href = tr.dataset.href;
  });
  renderRows();

  // ---- fill-ups -------------------------------------------------------------------
  app.appendChild(el("h2", "", "Fill-ups"));
  if (!fillups.length) {
    app.appendChild(el("p", "empty", "No fill-ups marked yet. Tap Filled up on the Fuel page after filling to the brim."));
    return;
  }
  const usable = fillups.filter((f) => f.usable && has(f.litres) && has(f.raw_l) && f.raw_l > 0);
  const k = usable.length ? sum(usable.map((f) => f.litres)) / sum(usable.map((f) => f.raw_l)) : null;
  app.appendChild(el("p", "hint", usable.length
    ? `Litres pumped against the raw estimate: k = ${fmt(k, 3)} over ${usable.length} fill-up${usable.length > 1 ? "s" : ""} ` +
      "(the line). Set this as the calibration once there are a few."
    : "The first fill-up only marks a starting point, and demo rows don't count: no calibration points yet."));
  if (usable.length) {
    const c = el("div", "card");
    app.appendChild(c);
    Charts.scatter(c, {
      points: usable.map((f) => [f.raw_l, f.litres]), color: C1, r: 4, slope: k, height: 240,
      xLabel: "Raw estimate since the last fill-up (L)", yLabel: "Litres pumped",
      tip: (i) => `<b>Fill-up ${fmt(usable[i].fill)}</b><br>${fmt(usable[i].litres, 1)} L pumped<br>${fmt(usable[i].raw_l, 2)} L estimated` +
        `<br>${fmt(usable[i].km, 1)} km`,
    });
    app.appendChild(el("div", "", "&nbsp;"));
  }
  const rows = fillups.map((f) => `<tr><td class="l">${fmt(f.fill)}</td><td class="l">${esc(f.time ? new Date(f.time).toLocaleString() : "–")}</td>` +
    `<td class="l">${esc(f.ride)}</td><td>${fmt(f.km, 1)}</td><td>${fmt(f.litres, 1)}</td><td>${fmt(f.raw_l, 2)}</td>` +
    `<td>${has(f.litres) && f.raw_l ? fmt(f.litres / f.raw_l, 3) : "–"}</td><td>${has(f.coverage_pct) ? fmt(f.coverage_pct) + " %" : "–"}</td>` +
    `<td class="l ${f.usable ? "" : "dim"}">${f.usable ? "yes" : f.ride === "demo" ? "no (demo)" : "no (starting point)"}</td></tr>`).join("");
  app.appendChild(el("div", "table-wrap", `<table><thead><tr><th class="l">#</th><th class="l">When</th><th class="l">Ride</th>` +
    `<th>km</th><th>Litres</th><th>Raw L</th><th>Ratio</th><th>Coverage</th><th class="l">Calibration point</th></tr></thead>` +
    `<tbody>${rows}</tbody></table>`));
})();
