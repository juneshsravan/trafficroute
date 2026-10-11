// traffic_layer.js — congestion, closures and verified incidents for the
// visible map area. Every segment carries its source; simulated data is
// labelled as simulated in the popup.
/* global L */

import { api } from "./api.js";
import { incidentIcon } from "./map.js";

function colourFor(seg) {
  if (seg.closed) return "#22262b";
  if (seg.delay_ratio >= 2) return "#c7362f";
  if (seg.delay_ratio >= 1.3) return "#e08a1e";
  return "#2e8b57";
}

function sourceLabel(seg) {
  if (seg.simulated) return "Simulated (demo data, not real traffic)";
  if (seg.source === "live") return "Live traffic feed";
  if (seg.source === "admin") return "Operator update";
  return "Verified incident";
}

export function createTrafficLayer(handles) {
  let controller = null;
  let enabledTraffic = true;
  let enabledIncidents = true;

  async function refresh() {
    const { map, trafficLayer, incidentLayer } = handles;
    if (controller) controller.abort();
    controller = new AbortController();
    const b = map.getBounds();
    let bbox = [b.getWest(), b.getSouth(), b.getEast(), b.getNorth()];
    // keep requests within the API's 25 square-degree limit
    if ((bbox[2] - bbox[0]) * (bbox[3] - bbox[1]) > 25) {
      trafficLayer.clearLayers();
      return;
    }
    bbox = bbox.map((v) => Math.round(v * 1e5) / 1e5);
    try {
      const [segs, incs] = await Promise.all([
        enabledTraffic ? api.trafficSegments(bbox, controller.signal) : null,
        enabledIncidents ? api.incidents(controller.signal) : null,
      ]);
      trafficLayer.clearLayers();
      incidentLayer.clearLayers();
      if (segs) {
        for (const s of segs.segments) {
          const line = L.polyline(s.geometry, {
            color: colourFor(s), weight: s.closed ? 7 : 5, opacity: 0.85,
            dashArray: s.closed ? "2 8" : null,
          }).addTo(trafficLayer);
          const road = s.road ? `<strong>${escapeHtml(s.road)}</strong><br>` : "";
          const state = s.closed ? "Closed to routing" :
            `${Math.round(s.current_seconds)} s instead of ${Math.round(s.baseline_seconds)} s (×${s.delay_ratio})`;
          line.bindPopup(`${road}${state}<br><em>${sourceLabel(s)}</em>`);
        }
      }
      if (incs) {
        for (const inc of incs.incidents) {
          const m = L.marker([inc.latitude, inc.longitude], { icon: incidentIcon(), title: inc.type }).addTo(incidentLayer);
          const desc = inc.description ? `<br>${escapeHtml(inc.description)}` : "";
          m.bindPopup(`<strong>${escapeHtml(capitalise(inc.type))}</strong>${desc}<br>Effect: ${escapeHtml(inc.routing_effect)}` +
                      (inc.simulated ? "<br><em>Simulated incident</em>" : ""));
        }
      }
    } catch (e) {
      if (e.name !== "AbortError") console.warn("traffic layer:", e.message);
    }
  }

  let timer = null;
  handles.map.on("moveend", () => { clearTimeout(timer); timer = setTimeout(refresh, 250); });

  return {
    refresh,
    setTraffic(on) { enabledTraffic = on; if (!on) handles.trafficLayer.clearLayers(); refresh(); },
    setIncidents(on) { enabledIncidents = on; if (!on) handles.incidentLayer.clearLayers(); refresh(); },
  };
}

function capitalise(s) { return s.charAt(0).toUpperCase() + s.slice(1); }

export function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
