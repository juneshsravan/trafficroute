# IndiaRoute AI — Architecture

All custom backend code is C11 (no C++, Python, Node or other backend
language). The browser frontend is HTML/CSS/JavaScript and only displays what
the C API returns — no routing logic runs in JavaScript.

```
 Browser: Leaflet map + planner (frontend/)
   │  fetch() JSON, same origin
   ▼
 ira_server (C) ──────────────────────────────────────────────────────────
   http_server.c   libmicrohttpd thread pool · 64 KB body cap · static files
                   · CSP/security headers · optional CORS · access log
   api.c           endpoint handlers · strict JSON validation (cJSON)
                   · rate limiting (rate_limit.c) · admin bearer auth
   app.c           shared state · rwlock · route-worker pool · provider thread
     ├─ spatial_index.c   grid index: nearest node / nearest edge
     ├─ routing.c         search loop, workspaces, penalties, reconstruction
     │    ├─ dijkstra.c   heuristic = 0
     │    └─ astar.c      haversine / v_max heuristic
     ├─ route_alternatives.c  penalty method
     ├─ route_geometry.c      polyline + basic turn instructions
     ├─ traffic.c        observations · incidents · expiry · simulated mode
     ├─ traffic_feed.c   libcurl provider polling · normalised feed ingest
     ├─ location_search.c  SQLite FTS5 place search
     ├─ database.c       SQLite data-access layer (parameter binding only)
     └─ graph.c / graph_bin.c  CSR graph, binary file I/O
 ─────────────────────────────────────────────────────────────────────────
 ira_import (C, offline)  readosm → osm_parser.c → graph_builder.c
                          → import_validation.c (SCC) → graph.bin + SQLite
```

## Key decisions

**Graph representation.** CSR arrays: edges sorted by source node,
`first_out[v]..first_out[v+1]` are v's outgoing edges. Junctions are graph
nodes; intermediate OSM points are shape geometry. Per-edge distance/time are
`float` (rounded *up* so stored distances never undercut the straight line),
path totals are `double`. No adjacency matrix anywhere.

**Two costs.** `distance_m` (metres) for *shortest*, effective time
(seconds: traffic override if present, else baseline) for *fastest*. Units
are fixed in the types and converted only at the input boundary
(km/h ÷ 3.6 = m/s).

**One search loop.** Dijkstra and A* share `route_search()`; they differ only
in the heuristic. Tests check A* equals Dijkstra (and an independent
Bellman-Ford) on hundreds of random graphs.

**Admissible A\*.** Distance mode: haversine to target. Time mode: haversine
÷ fastest effective speed in the graph, an upper bound kept up to date as
traffic changes. Both are consistent, so settled nodes are never reopened.

**Workspaces.** Per-node scratch arrays with generation stamps: starting a
query is O(1), not O(V). The server keeps a fixed pool
(`IRA_ROUTE_WORKERS`), which bounds memory and concurrency; requests wait up
to 5 s for a worker, then get `503`.

**Traffic is separate from routing.** `traffic.c` only writes each edge's
`traffic_time_s` and `CLOSED` flag; later searches see them. Observations
have a source (live / admin / simulated), confidence and expiry; expired data
is removed and weights recomputed. Every route response states where its
travel times came from. Unverified incident reports never change routes.

**Concurrency.** Topology, coordinates and indexes are immutable after start-up.
Traffic weights are guarded by a pthread rwlock: searches hold it for
reading, updates for writing.

**Persistence.** The router reads `graph.bin` (validated, checksummed,
atomically replaced). SQLite holds places (FTS5), import metadata, incidents
(restored on restart and re-matched by coordinate), and a traffic audit log.
`database.c` is the only SQL code, so a PostgreSQL/PostGIS port changes one
file.

## Prototype vs. nationwide

| Concern | Now | For all-India production |
|---|---|---|
| Graph storage | compact CSR in RAM, loaded from graph.bin | same; mmap loading to cut start-up time |
| Search speed | Dijkstra / A* (~1 s on 4 M nodes, measured) | Contraction Hierarchies or ALT preprocessing in the importer |
| Snapping | uniform grid, main-component preference | same (grid scales; cell size adapts) |
| Concurrency | thread pool + bounded worker pool | multiple server processes behind a proxy |
| Search memory | ~37 B/node per worker | same; size the pool to RAM |
| Request bounds | 64 KB body, `IRA_MAX_SETTLED`, rate limits | same + proxy limits |
| Graph updates | atomic file replace + restart | blue/green restart of processes |

See [data_pipeline.md](data_pipeline.md), [api.md](api.md),
[benchmarks.md](benchmarks.md), [deployment.md](deployment.md).

## Known limitations

* Turn restrictions are not applied yet; instructions are "basic".
* Snapping is to the nearest junction/shape-split node (≤ 1 km apart), not to
  an arbitrary point mid-edge.
* No live traffic provider is connected; only the provider interface and a
  normalised feed format exist. Simulated traffic is always labelled.
* State/district for places come from OSM tags where present, not from
  boundary polygons.
* All-India import has not been run in the development environment
  (no access to Geofabrik there); real coverage and performance must be
  measured with `scripts/import_osm.sh india`.
