// map.js — Leaflet map, endpoint pins and layer groups.
/* global L */

export function createMap(el, config) {
  const map = L.map(el, { zoomControl: false, attributionControl: true, preferCanvas: false });
  L.control.zoom({ position: "topright" }).addTo(map);

  // Tile source comes from the server config (IRA_TILE_URL). The public OSM
  // tile server is for light development use only (see its usage policy).
  L.tileLayer(config.tile_url, {
    maxZoom: 19,
    attribution: config.tile_attribution,
  }).addTo(map);
  // Avoid crediting OpenStreetMap twice when the tiles already do.
  if (config.data_attribution) {
    const dup = /OpenStreetMap/.test(config.tile_attribution) && /OpenStreetMap/.test(config.data_attribution);
    map.attributionControl.addAttribution(dup ? "Road data ODbL" : config.data_attribution);
  }

  const [minLat, minLon, maxLat, maxLon] = config.bbox;
  map.fitBounds([[minLat, minLon], [maxLat, maxLon]], { padding: [30, 30] });

  return {
    map,
    routeLayer: L.layerGroup().addTo(map),
    trafficLayer: L.layerGroup().addTo(map),
    incidentLayer: L.layerGroup().addTo(map),
    markerLayer: L.layerGroup().addTo(map),
  };
}

function pinIcon(kind) {
  return L.divIcon({
    className: "",
    html: `<div class="pin pin-${kind}"></div>`,
    iconSize: [26, 26],
    iconAnchor: [13, 26],
  });
}

export function placePin(layer, latlng, kind, title) {
  return L.marker(latlng, { icon: pinIcon(kind), title, keyboard: true }).addTo(layer);
}

export function incidentIcon() {
  return L.divIcon({ className: "", html: '<div class="incident-dot"></div>', iconSize: [18, 18], iconAnchor: [9, 9] });
}
