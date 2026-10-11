# REST API (v1)

Implemented in C (`backend/src/api.c`, served by libmicrohttpd in
`http_server.c`). Machine-readable spec: [`openapi.yaml`](openapi.yaml).

Conventions

* Base path `/api/v1`. JSON in and out (`Content-Type: application/json`
  is required on POST, otherwise `415`).
* Success: `{"status":"success", ...}`.
  Error: `{"status":"error","error":{"code":"...","message":"...","field":"..."}}`.
* Request bodies are validated strictly: unknown fields, wrong types and
  out-of-range values give `400` — they are never silently ignored.
* Body limit 64 KB (`413 BODY_TOO_LARGE`).
* Rate limits per client IP (token buckets): general 300/min, route
  requests 60/min, incident reports 10/hour (configurable). Exceeding them
  gives `429 RATE_LIMITED` with a `Retry-After` header.
* Coordinates are WGS84 `latitude`/`longitude` in degrees; distances in
  metres, durations in seconds.

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | server, graph, database, traffic-provider status |
| GET | `/config` | tile URL, attribution, dataset bbox (used by the frontend) |
| GET | `/network/stats` | graph size, road classes, memory, import report, coverage note |
| GET | `/locations/search?q=&limit=&offset=` | place search (local FTS5 index) |
| GET | `/locations/snap?lat=&lon=` | nearest routable road node |
| POST | `/routes` | best route (+ optional alternatives) |
| POST | `/routes/shortest` | minimum-distance route |
| POST | `/routes/alternatives` | best route + ≥1 alternatives (default 2) |
| GET | `/traffic/segments?bbox=minLon,minLat,maxLon,maxLat` | segments whose weight is currently changed |
| GET | `/incidents` | verified, active incidents |
| POST | `/incidents` | report an incident (pending moderation) |
| POST | `/admin/traffic/updates` | operator travel-time updates — **admin** |
| POST | `/admin/traffic/simulate` | labelled simulated congestion — **admin** |
| DELETE | `/admin/traffic?source=simulated\|admin\|live` | clear observations — **admin** |
| GET | `/admin/incidents?status=pending\|verified\|rejected\|expired\|all` | moderation queue — **admin** |
| POST | `/admin/incidents` | create an incident (may set `verified`/`simulated`) — **admin** |
| POST | `/admin/incidents/{id}/verify` / `.../reject` | moderate — **admin** |

Admin endpoints need `Authorization: Bearer $IRA_ADMIN_TOKEN` (≥ 16 chars).
Without a configured token they return `403 ADMIN_DISABLED`; a wrong or
missing token gives `401 UNAUTHORIZED`. The token is compared in constant time
and never logged.

## POST /routes

```json
{
  "origin":      {"latitude": 43.7310, "longitude": 7.4248},
  "destination": {"latitude": 43.7500, "longitude": 7.4400},
  "mode": "fastest",            // "fastest" (time) | "shortest" (distance); default fastest
  "algorithm": "dijkstra",      // "dijkstra" (default) | "astar" — same optimal result
  "alternatives": 2,            // 0-3, default 0 (2 on /routes/alternatives)
  "avoid": {"tolls": false, "restricted": false},
  "include_polyline": false     // also return Google encoded polyline (precision 1e5)
}
```

Response `200` (real output from the Monaco test extract, geometry shortened):

```json
{
  "status": "success",
  "routing_mode": "fastest",
  "algorithm": "dijkstra",
  "distance_meters": 4215.2,
  "estimated_duration_seconds": 363.3,
  "traffic_data": {
    "available": false, "live": false, "simulated": false,
    "admin_overrides": false, "incident_delay_model": false, "closed_segments": 0,
    "message": "No live traffic data - using baseline travel-time estimates from road type and speed limits."
  },
  "snapped": {
    "origin": {"latitude": 43.730774, "longitude": 7.425064, "snap_distance_meters": 32.9},
    "destination": {"latitude": 43.750338, "longitude": 7.439766, "snap_distance_meters": 42}
  },
  "route": {
    "distance_meters": 4215.2,
    "estimated_duration_seconds": 363.3,
    "baseline_duration_seconds": 363.3,
    "traffic_affected_segments": 0,
    "segment_count": 53,
    "nodes_settled": 976,
    "geometry": [[43.730774, 7.425064], [43.73089, 7.42531], "..."],
    "instructions": [
      {"type": "depart", "text": "Head northeast on Avenue Saint-Martin", "road": "Avenue Saint-Martin",
       "distance_meters": 209, "duration_seconds": 30.1, "location": [43.730774, 7.425064], "geometry_index": 0},
      {"type": "continue", "text": "Continue onto Avenue de la Porte Neuve", "...": "..."},
      {"type": "arrive", "text": "Arrive at your destination", "...": "..."}
    ],
    "waypoints": [[43.730774, 7.425064], [43.750338, 7.439766]]
  },
  "alternatives": [ {"distance_meters": 4724, "estimated_duration_seconds": 391.9, "...": "same shape as route"} ],
  "instructions_note": "Basic instructions derived from road names and bearings; turn restrictions and lanes are not modelled.",
  "compute_ms": 0.4,
  "dataset": "osm:monaco.osm.pbf (file date 2026-10-10)",
  "attribution": "Road data (c) OpenStreetMap contributors, ODbL"
}
```

`traffic_data` is always present and always says where travel times come
from. `simulated: true` means demo data is active; the message then starts
with "SIMULATED traffic for demonstration only".

Errors

| Status | Code | When |
|---|---|---|
| 400 | `EMPTY_BODY`, `INVALID_JSON` | missing / malformed body, not an object |
| 400 | `MISSING_FIELD`, `INVALID_FIELD`, `UNKNOWN_FIELD` | field problems (`field` names it) |
| 400 | `INVALID_COORDINATES` | lat ∉ [-90,90], lon ∉ [-180,180], non-numeric |
| 400 | `INVALID_MODE` | mode not fastest/shortest |
| 404 | `NO_ROUTE` | points on disconnected roads, or all connections closed/avoided |
| 422 | `OUTSIDE_COVERAGE` | point outside the loaded dataset's area |
| 422 | `NOT_ON_NETWORK` | no road within `IRA_MAX_SNAP_M` (5 km) |
| 422 | `ROUTE_TOO_COMPLEX` | search exceeded `IRA_MAX_SETTLED` |
| 503 | `SERVER_BUSY` | all route workers busy for 5 s (`Retry-After: 2`) |

## GET /locations/search

`q` (1–100 chars, required), `limit` (1–25, default 10), `offset` (0–1000).
Words are prefix-matched in any script (`Bengal` finds Bengaluru). Ranking:
exact name match, then place importance (city > state/district > town > suburb
> village > hamlet), then relevance, then population.

```json
{"status":"success","query":"mona","count":2,"results":[
  {"id":19,"name":"Monaco","type":"city","label":"Monaco","latitude":43.731142,
   "longitude":7.419758,"population":36371,"routable":true,"snap_distance_meters":102.6},
  {"id":6,"name":"Monaco-Ville","type":"suburb","near":"Monaco","label":"Monaco-Ville, near Monaco",
   "latitude":43.73097,"longitude":7.424815,"routable":true,"snap_distance_meters":29.6}]}
```

`district`/`state` appear when the OSM place node carries them
(`addr:state`, `is_in:state`, `addr:district`, ...); `near` is the nearest
city/town within 50 km for smaller places. `routable` is only `true` when the
place snaps to the road network.

## Traffic

`GET /traffic/segments?bbox=` (area ≤ 25 square degrees, ≤ 2000 segments):

```json
{"segments":[{"edge_index":812,"road":"Boulevard Albert 1er","closed":false,
  "baseline_seconds":21.3,"current_seconds":52.4,"delay_ratio":2.46,
  "source":"simulated","simulated":true,"confidence":1,"observed_at":1791634513,
  "expires_at":1791638113,"geometry":[[43.73,7.42],"..."]}], "count":1, "traffic_data":{"...":"..."}}
```

`POST /admin/traffic/updates`:

```json
{"updates":[
  {"edge_index": 812, "travel_time_seconds": 60, "expires_in_seconds": 900},
  {"latitude": 43.7347, "longitude": 7.4216, "speed_kmh": 12, "confidence": 0.8}
]}
```
Each item needs `edge_index` or a coordinate (matched to the nearest road
within 50 m) and `travel_time_seconds` or `speed_kmh`. Up to 1000 per call;
per-item results say which were accepted.

`POST /admin/traffic/simulate`: `{"latitude","longitude","radius_m"(50–50000),"count"(1–500),"duration_seconds","seed"}`.
Creates congestion ×1.3–3.5 on random roads, source `simulated`.

### Live provider feed

When `IRA_TRAFFIC_FEED_URL` is set the server polls it (libcurl; 10 s timeout,
3 retries with 1/2/4 s back-off, HTTP 429 `Retry-After` honoured, 16 MB cap,
`Authorization: Bearer $IRA_TRAFFIC_API_KEY` if set). The feed must use this
normalised format:

```json
{"provider":"name","attribution":"text to show users","observations":[
  {"latitude":12.97,"longitude":77.59,"speed_kmh":18.5,"confidence":0.8,
   "observed_at":1760000000,"expires_at":1760000900}]}
```
Observations older than 30 min, from the future, or more than 40 m from a road
are rejected. Commercial APIs (TomTom, HERE, ...) need a small adapter that
converts their format, plus their own keys, quotas and licence terms — none is
built in.

## Incidents

`POST /incidents` (public): `{"type":"accident|closure|congestion|roadwork|hazard",
"latitude","longitude","description"(≤200),"expires_in_minutes"(5–1440, default 120),
"both_directions"(default true)}` → `202` with status `pending`. Pending reports
are not shown publicly and never change routes. After an admin verifies one:

| type | routing effect |
|---|---|
| closure | edge(s) closed — routes avoid them |
| accident | travel time × `IRA_ACCIDENT_DELAY_FACTOR` (2.0) — an estimated **model** |
| roadwork | travel time × `IRA_ROADWORK_DELAY_FACTOR` (1.5) — an estimated **model** |
| congestion, hazard | informational only |

Incidents expire automatically, are persisted in SQLite, and are re-attached
to the road network by coordinate when the server restarts.
