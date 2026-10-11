// route_display.js — draws routes returned by the C API and fills the panel.
/* global L */

export function formatDistance(m) {
  if (m < 1000) return `${Math.round(m)} m`;
  return m < 100000 ? `${(m / 1000).toFixed(1)} km` : `${Math.round(m / 1000)} km`;
}

export function formatDuration(s) {
  const min = Math.round(s / 60);
  if (min < 1) return "under 1 min";
  if (min < 60) return `${min} min`;
  const h = Math.floor(min / 60), r = min % 60;
  return r ? `${h} h ${r} min` : `${h} h`;
}

const STYLE_MAIN = { color: "#1e4fcb", weight: 6, opacity: 0.92, lineCap: "round", lineJoin: "round" };
const STYLE_CASING = { color: "#ffffff", weight: 10, opacity: 0.9, lineCap: "round", lineJoin: "round" };
const STYLE_ALT = { color: "#5b6470", weight: 5, opacity: 0.75, dashArray: "8 9", lineCap: "round" };

// Draws the selected route on top and the others as dashed grey lines that
// can be clicked to select them.
export function drawRoutes(layer, routes, selected, onSelect) {
  layer.clearLayers();
  routes.forEach((r, i) => {
    if (i === selected) return;
    const line = L.polyline(r.geometry, STYLE_ALT).addTo(layer);
    line.bindTooltip(`${formatDuration(r.estimated_duration_seconds)}, ${formatDistance(r.distance_meters)}`, { sticky: true });
    line.on("click", () => onSelect(i));
  });
  const main = routes[selected];
  L.polyline(main.geometry, STYLE_CASING).addTo(layer);
  return L.polyline(main.geometry, STYLE_MAIN).addTo(layer);
}

export function mainRoad(route) {
  // Longest named leg, for the milestone cap (like the road name on a stone).
  let best = null;
  for (const s of route.instructions) {
    if (!s.road || s.road.startsWith("unnamed")) continue;
    if (!best || s.distance_meters > best.distance_meters) best = s;
  }
  return best ? best.road : "Your route";
}

export function renderSteps(ol, route, onStepClick) {
  ol.innerHTML = "";
  route.instructions.forEach((s) => {
    const li = document.createElement("li");
    li.tabIndex = 0;
    const text = document.createElement("span");
    text.textContent = s.text;
    const dist = document.createElement("span");
    dist.className = "dist";
    dist.textContent = s.type === "arrive" ? "" : formatDistance(s.distance_meters);
    li.append(text, dist);
    const go = () => onStepClick(s.location);
    li.addEventListener("click", go);
    li.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); go(); } });
    ol.appendChild(li);
  });
}

export function renderAlternatives(ul, routes, selected, onSelect) {
  ul.innerHTML = "";
  const best = routes[selected];
  routes.forEach((r, i) => {
    if (i === selected) return;
    const li = document.createElement("li");
    const b = document.createElement("button");
    b.type = "button";
    const label = document.createElement("span");
    label.textContent = `${formatDuration(r.estimated_duration_seconds)}, ${formatDistance(r.distance_meters)}`;
    const delta = document.createElement("span");
    delta.className = "delta";
    const dt = r.estimated_duration_seconds - best.estimated_duration_seconds;
    delta.textContent = dt >= 0 ? `${formatDuration(dt)} slower` : `${formatDuration(-dt)} faster`;
    b.append(label, delta);
    b.addEventListener("click", () => onSelect(i));
    li.appendChild(b);
    ul.appendChild(li);
  });
}
