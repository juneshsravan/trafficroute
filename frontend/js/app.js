// app.js — wires the planner UI to the C routing API.

import { api } from "./api.js";
import { createMap, placePin } from "./map.js";
import { attachSearch } from "./search.js";
import { drawRoutes, renderSteps, renderAlternatives, formatDistance, formatDuration, mainRoad } from "./route_display.js";
import { createTrafficLayer } from "./traffic_layer.js";

const $ = (id) => document.getElementById(id);

const state = {
  origin: null,       // {lat, lon, label}
  dest: null,
  routes: [],
  selected: 0,
  lastResponse: null,
  reportPoint: null,
  reporting: false,
};

let handles, traffic, originPin, destPin, reportPin;

function setError(msg) {
  const el = $("error");
  el.hidden = !msg;
  el.textContent = msg || "";
}

function updateGo() {
  const ready = state.origin && state.dest;
  $("go").disabled = !ready;
  $("hint").textContent = !state.origin ? "Choose a start: search or click the map."
    : !state.dest ? "Now choose a destination: search or click the map."
    : "Ready. Find the route.";
}

function setPoint(which, pt) {
  state[which] = pt;
  const layer = handles.markerLayer;
  if (which === "origin") {
    if (originPin) layer.removeLayer(originPin);
    originPin = pt ? placePin(layer, [pt.lat, pt.lon], "start", "Start") : null;
  } else {
    if (destPin) layer.removeLayer(destPin);
    destPin = pt ? placePin(layer, [pt.lat, pt.lon], "end", "Destination") : null;
  }
  updateGo();
}

async function onMapClick(e) {
  const { lat, lng } = e.latlng;
  if (state.reporting) {
    state.reportPoint = { lat, lon: lng };
    if (reportPin) handles.markerLayer.removeLayer(reportPin);
    reportPin = placePin(handles.markerLayer, [lat, lng], "report", "Reported problem");
    $("report-where").textContent = `Location: ${lat.toFixed(5)}, ${lng.toFixed(5)}`;
    $("report-send").disabled = false;
    return;
  }
  const which = !state.origin ? "origin" : !state.dest ? "dest" : "dest";
  // Confirm the click is near a road before accepting it.
  try {
    const snap = await api.snap(lat.toFixed(6), lng.toFixed(6));
    if (!snap.routable) {
      setError("There is no road close to that point in the loaded map data. Click nearer to a road.");
      return;
    }
    setError("");
    const label = `Map point ${lat.toFixed(4)}, ${lng.toFixed(4)}`;
    setPoint(which, { lat, lon: lng, label });
    $(which === "origin" ? "origin-input" : "dest-input").value = label;
    if (state.origin && state.dest) findRoute();
  } catch (err) {
    setError(err.message);
  }
}

function showTrafficBanner(td) {
  const el = $("traffic-banner");
  el.hidden = false;
  el.textContent = td.message;
  el.dataset.kind = td.simulated ? "simulated" : td.live ? "live" : "baseline";
}

function selectRoute(i) {
  state.selected = i;
  const r = state.routes[i];
  const line = drawRoutes(handles.routeLayer, state.routes, i, selectRoute);
  handles.map.fitBounds(line.getBounds(), { padding: [40, 40] });

  const km = r.distance_meters / 1000;
  $("milestone-km").textContent = km < 10 ? km.toFixed(1) : Math.round(km).toString();
  $("milestone-time").textContent = formatDuration(r.estimated_duration_seconds);
  $("milestone-road").textContent = mainRoad(r);
  const ms = $("milestone");
  ms.classList.remove("arrive");
  void ms.offsetWidth;          // restart the one-shot animation
  ms.classList.add("arrive");

  const resp = state.lastResponse;
  const delay = r.estimated_duration_seconds - r.baseline_duration_seconds;
  const parts = [
    `${resp.routing_mode === "fastest" ? "Fastest" : "Shortest"} route by ${resp.algorithm === "astar" ? "A*" : "Dijkstra"}`,
    `computed in ${resp.compute_ms} ms`,
  ];
  if (Math.abs(delay) >= 30) parts.push(`${formatDuration(Math.abs(delay))} ${delay > 0 ? "of delay" : "saved"} versus baseline`);
  $("route-meta").textContent = parts.join(", ") + ".";

  renderSteps($("steps"), r, (loc) => handles.map.setView(loc, Math.max(handles.map.getZoom(), 16)));
  $("steps-note").textContent = resp.instructions_note;
  $("alts").hidden = state.routes.length < 2;
  renderAlternatives($("alt-list"), state.routes, i, selectRoute);
}

async function findRoute() {
  if (!state.origin || !state.dest) return;
  const go = $("go");
  go.setAttribute("aria-busy", "true");
  go.textContent = "Finding route…";
  setError("");
  try {
    const body = {
      origin: { latitude: state.origin.lat, longitude: state.origin.lon },
      destination: { latitude: state.dest.lat, longitude: state.dest.lon },
      mode: document.querySelector('input[name="mode"]:checked').value,
      algorithm: $("algorithm").value,
      alternatives: Number($("alternatives").value),
      avoid: { tolls: $("avoid-tolls").checked, restricted: $("avoid-restricted").checked },
    };
    const resp = await api.route(body);
    state.lastResponse = resp;
    state.routes = [resp.route, ...resp.alternatives];
    $("result").hidden = false;
    showTrafficBanner(resp.traffic_data);
    selectRoute(0);
    traffic.refresh();
  } catch (e) {
    $("result").hidden = true;
    handles.routeLayer.clearLayers();
    setError(e.message);
  } finally {
    go.removeAttribute("aria-busy");
    go.textContent = "Find route";
  }
}

function swap() {
  const o = state.origin, d = state.dest;
  const ov = $("origin-input").value, dv = $("dest-input").value;
  setPoint("origin", d);
  setPoint("dest", o);
  $("origin-input").value = dv;
  $("dest-input").value = ov;
  if (state.origin && state.dest) findRoute();
}

function setupReporting() {
  const form = $("report-form");
  $("report-btn").addEventListener("click", () => {
    state.reporting = true;
    form.hidden = false;
    $("report-btn").hidden = true;
    $("report-status").textContent = "";
  });
  const stop = () => {
    state.reporting = false;
    form.hidden = true;
    $("report-btn").hidden = false;
    state.reportPoint = null;
    $("report-send").disabled = true;
    $("report-where").textContent = "No location chosen yet.";
    if (reportPin) { handles.markerLayer.removeLayer(reportPin); reportPin = null; }
  };
  $("report-cancel").addEventListener("click", stop);
  form.addEventListener("submit", async (e) => {
    e.preventDefault();
    if (!state.reportPoint) return;
    try {
      const resp = await api.reportIncident({
        type: $("report-type").value,
        latitude: state.reportPoint.lat,
        longitude: state.reportPoint.lon,
        description: $("report-desc").value.trim(),
      });
      stop();
      $("report-desc").value = "";
      $("report-status").textContent = resp.message;
    } catch (err) {
      $("report-status").textContent = err.message;
    }
  });
}

async function main() {
  let config;
  try {
    config = await api.config();
  } catch (e) {
    setError(e.message);
    return;
  }
  handles = createMap($("map"), config);
  traffic = createTrafficLayer(handles);
  handles.map.on("click", onMapClick);

  if (config.synthetic_sample) {
    const note = $("dataset-note");
    note.hidden = false;
    note.textContent = "Demo mode: the server is using a small synthetic test network, not real roads. " +
      "Import OpenStreetMap data with ira_import to route on real Indian roads.";
  }
  const credits = $("credits");
  credits.innerHTML = "";
  for (const text of [`Dataset: ${config.dataset}`, "Routes computed by the IndiaRoute C engine."]) {
    const p = document.createElement("p");
    p.textContent = text;
    credits.appendChild(p);
  }

  attachSearch($("origin-input"), $("origin-list"), (pt) => { setPoint("origin", pt); if (state.dest) findRoute(); });
  attachSearch($("dest-input"), $("dest-list"), (pt) => { setPoint("dest", pt); if (state.origin) findRoute(); });
  $("swap").addEventListener("click", swap);
  $("planner").addEventListener("submit", (e) => { e.preventDefault(); findRoute(); });
  document.querySelectorAll('input[name="mode"], #avoid-tolls, #avoid-restricted, #alternatives, #algorithm')
    .forEach((el) => el.addEventListener("change", () => { if (state.origin && state.dest) findRoute(); }));
  $("show-traffic").addEventListener("change", (e) => traffic.setTraffic(e.target.checked));
  $("show-incidents").addEventListener("change", (e) => traffic.setIncidents(e.target.checked));
  $("sheet-toggle").addEventListener("click", () => {
    const rail = $("rail");
    const collapsed = rail.classList.toggle("collapsed");
    $("sheet-toggle").setAttribute("aria-expanded", String(!collapsed));
    $("sheet-toggle").textContent = collapsed ? "Show planner" : "Hide planner";
    handles.map.invalidateSize();
  });
  setupReporting();
  updateGo();
  traffic.refresh();

  try {
    const h = await api.health();
    showTrafficBanner(h.traffic_data);
  } catch { /* banner stays hidden */ }
}

main();
