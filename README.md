# IndiaRoute AI

A traffic-aware route planner for Indian roads. The backend — HTTP server,
REST API, graph engine, Dijkstra, A*, alternative routes, traffic and incident
handling, OpenStreetMap importer — is written entirely in **C11**. The
browser frontend (Leaflet) only displays what the C API returns.

## Status

| Area | State |
|---|---|
| Graph engine (CSR), Dijkstra, A*, shortest + fastest | ✅ implemented, tested against Bellman-Ford on random graphs |
| One-way, tolls, restricted roads, closures, traffic weights | ✅ |
| Alternative routes (penalty method) | ✅ |
| C HTTP server + REST API (libmicrohttpd, cJSON) | ✅ all endpoints in [docs/api.md](docs/api.md) |
| Leaflet frontend (search, map clicks, alternatives, directions, traffic/incident layers, reporting) | ✅ |
| OSM importer in C (readosm), place search index (SQLite FTS5) | ✅ tested on a real OSM extract (Monaco) |
| Traffic: admin updates, simulated mode, incident moderation, expiry, live-feed interface (libcurl) | ✅ — **no live provider connected** |
| Security: strict validation, rate limits, admin token, CSP, sanitizer-clean | ✅ |
| All-India data | ⚠️ importer ready; **not yet imported or measured** (see below) |
| Turn restrictions | ❌ not yet applied |

**Coverage claim:** none yet for India. The development sandbox could not
download Geofabrik data, so the pipeline was verified on the real Monaco
extract and on synthetic graphs up to 4 million nodes. Run
`scripts/import_osm.sh southern-zone` (or `india`) to import real Indian
roads; the import report and `bench_route` give measured coverage and speed.

## Quick start (Ubuntu / WSL2)

```bash
sudo apt install build-essential pkg-config libmicrohttpd-dev libcurl4-openssl-dev \
                 libsqlite3-dev libreadosm-dev
scripts/build.sh             # compile + run tests
scripts/run.sh monaco        # real OSM test data (Monaco), opens on http://127.0.0.1:8080
# or
scripts/run.sh               # synthetic sample network (clearly labelled as such)
```

On Windows use WSL2: open "Ubuntu", `cd` to the project, run the commands
above, then open http://localhost:8080 in your Windows browser.

## Real Indian road data

```bash
scripts/import_osm.sh southern-zone        # ~ South India, downloads from Geofabrik
IRA_GRAPH_PATH=data/processed/southern-zone.graph.bin \
IRA_DB_PATH=data/processed/southern-zone.sqlite scripts/run.sh
```
Data © OpenStreetMap contributors, ODbL 1.0. Memory planning formulas are in
[docs/data_pipeline.md](docs/data_pipeline.md).

## Configuration

Environment variables (see `.env.example`; `scripts/run.sh` loads `.env`):
`IRA_GRAPH_PATH`, `IRA_DB_PATH`, `IRA_PORT`, `IRA_BIND`, `IRA_ADMIN_TOKEN`
(≥ 16 chars to enable `/api/v1/admin/*`), `IRA_ROUTE_WORKERS`,
`IRA_MAX_SETTLED`, `IRA_TRAFFIC_FEED_URL`, `IRA_TRAFFIC_API_KEY`,
`IRA_TILE_URL`, rate limits, and more. Secrets are read only from the
environment and never logged.

## Try the API

```bash
curl -s -X POST localhost:8080/api/v1/routes -H 'Content-Type: application/json' -d '{
  "origin": {"latitude": 43.7310, "longitude": 7.4248},
  "destination": {"latitude": 43.7500, "longitude": 7.4400},
  "mode": "fastest", "alternatives": 2 }'

curl -s "localhost:8080/api/v1/locations/search?q=Monaco"

# demo traffic (labelled "SIMULATED" in every response)
curl -s -X POST localhost:8080/api/v1/admin/traffic/simulate \
  -H "Authorization: Bearer $IRA_ADMIN_TOKEN" -H 'Content-Type: application/json' \
  -d '{"latitude":43.738,"longitude":7.425,"radius_m":1500,"count":30}'
```

## Testing

```bash
cd backend
make test          # 13 suites, ~1,900 checks: heap, graph, Dijkstra, routing, spatial index,
                   # graph.bin, alternatives, geometry, traffic, REST API, feed, rate limit, OSM rules
make test-import   # real OSM extract -> graph -> 300 random routes -> HTTP server
make sanitize      # all tests under AddressSanitizer + UndefinedBehaviorSanitizer
make valgrind      # all tests under valgrind
```
All of these pass with GCC 13 and Clang, built with `-Wall -Wextra -Wpedantic -Wconversion -Werror`.

Acceptance criteria → where they are verified:

| # | Criterion | Test |
|---|---|---|
| 1 | C-only build | Makefile (`-std=c11`, no C++ sources) |
| 2–5 | correct shortest/fastest routes, node+edge reconstruction | `test_dijkstra`, `test_routing` (hand-checked + Bellman-Ford oracle) |
| 6 | disconnected destination → error | `test_dijkstra`, `test_api` (`NO_ROUTE` 404) |
| 7 | one-way / restrictions respected | `test_routing`, `test_osm_rules` |
| 8 | traffic update changes fastest route | `test_routing`, `test_traffic`, `test_api` |
| 9 | closure blocks segment | `test_routing`, `test_traffic`, `test_api` |
| 10 | frontend shows computed geometry | headless-browser check during development (route drawn from API geometry) |
| 11 | invalid input rejected safely | `test_api` (19 validation cases), sanitizers |
| 12 | routes on a real-world dataset | `make test-import` (OSM Monaco) — a regional **Indian** dataset still to be run |
| 13 | measure before claiming scale | [docs/benchmarks.md](docs/benchmarks.md); India not yet measured |
| 14 | honest about missing live traffic | `traffic_data` in every route response; `test_api`, `test_feed` |
| 15 | README covers build/config/data/run/test/deploy | this file + [docs/](docs/) |

## Layout

```
backend/
  include/, src/        C server + engine (main.c = ira_server, route_cli.c, bench_route.c)
  importer/             C OSM importer (ira_import)
  tests/                C unit/API tests + test_import.sh
  third_party/cjson/    vendored cJSON (MIT)
frontend/               index.html, css/, js/ (ES modules), vendor/ (Leaflet, Overpass font)
data/sample/            synthetic test network (documented)
data/test_osm/          real Monaco OSM extract + hand-written tag-rule test file
database/schema.sql     generated from backend/src/database.c
docs/                   architecture, api (+ openapi.yaml), data pipeline, benchmarks, deployment
scripts/                build.sh, run.sh, import_osm.sh
```

## Credits

Road data © OpenStreetMap contributors (ODbL). Leaflet (BSD-2), cJSON (MIT),
Overpass font (SIL OFL), libmicrohttpd (LGPL), libcurl, SQLite, readosm.
