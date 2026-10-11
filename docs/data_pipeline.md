# Road-data pipeline

`ira_import` (C, `backend/importer/`) turns an OpenStreetMap extract into
`graph.bin` (routing graph) and a SQLite file (place search index, import
metadata). It is an offline administrative step; the server never rebuilds
the graph during a request.

## Source data

| Extract | URL | Notes |
|---|---|---|
| All India | `https://download.geofabrik.de/asia/india-latest.osm.pbf` | ~1.6 GB (Geofabrik, Oct 2026), updated daily |
| Six zones | `https://download.geofabrik.de/asia/india/<zone>-latest.osm.pbf` | northern, southern, eastern, western, central, north-eastern |

Licence: **ODbL 1.0**. Any map or API output built from it must credit
"© OpenStreetMap contributors"; the API's `attribution` field and the
frontend do this.

`scripts/import_osm.sh southern-zone` downloads, imports, records peak memory
and runs `bench_route` on the result.

## Parser

[readosm](https://www.gaia-gis.it/fossil/readosm) — a C library that streams
both `.osm.pbf` and `.osm` XML through callbacks (Debian/Ubuntu package
`libreadosm-dev`). No C++ (osmium) and no Python. If readosm turns out too
slow for the full country, the documented fallback is decoding the official
`fileformat.proto`/`osmformat.proto` with protobuf-c + zlib.

## Two passes

1. **Pass 1** — every way is classified (rules below). Routable ways keep
   their node references and attributes; place nodes (city, town, village,
   hamlet, suburb, locality, state, district, ...) with a name are kept for
   the search index. All referenced node ids are then sorted, de-duplicated
   and counted.
2. **Pass 2** — coordinates are read only for referenced nodes (binary search
   in the sorted id array — no hash map, predictable memory). Geofabrik files
   are sorted (nodes before ways), so pass 2 stops at the first way;
   `--no-early-stop` reads unsorted files completely.
3. **Build** — each way is split at junctions (nodes used ≥ 2 times, or way
   ends). Each piece becomes one directed edge, or two for two-way roads;
   intermediate points become edge shape geometry, not graph nodes. Pieces
   longer than `--max-edge-length` (default 1000 m) are split at shape
   points so that clicks on long rural roads snap to a nearby node.
   Distance = sum of haversine lengths along the polyline (so it is never
   shorter than the straight line — needed for A*).
4. **Connectivity** — iterative Tarjan SCC (no recursion, safe for national
   graphs). Nodes of the largest strongly connected component get
   `NODE_FLAG_MAIN_COMPONENT`; snapping prefers them so a click beside an
   isolated fragment (a private compound, an unconnected new layout) does
   not produce "no route".
5. **Output** — `graph.bin` is written to `graph.bin.tmp` and renamed into
   place (atomic replacement); FNV-1a checksum, struct-size and version
   checks guard against corrupt or incompatible files. Places are written in
   one SQLite transaction and the FTS5 index rebuilt. A JSON report goes to
   `<output>.report.json` and into `import_metadata`.

## Tag rules (`importer/osm_parser.c`, tested in `tests/test_osm_rules.c`)

| Tag | Effect |
|---|---|
| `highway=motorway, trunk, primary, secondary, tertiary` (+ `_link`) | routable, class from tag; links at 75 % speed |
| `highway=unclassified, road` | class unclassified |
| `highway=residential, living_street` | class residential |
| `highway=service` | class service |
| `highway=track` | class track (on by default: many Indian village roads are tracks; `--no-tracks` drops them) |
| `highway=footway, path, cycleway, steps, pedestrian, construction, proposed, ...` | not routable |
| `motorcar` > `motor_vehicle` > `vehicle` > `access` = `no`/`private` | way dropped |
| same keys = `destination/delivery/customers/permit/agricultural/forestry` | `RESTRICTED` flag (avoidable) |
| `area=yes` | dropped |
| `oneway=yes/true/1` / `-1/reverse`; `motorway`, `motorway_link`, `junction=roundabout/circular` imply oneway | one directed edge |
| `toll=yes` | `TOLL` flag (avoidable) |
| `maxspeed=N`, `N mph` | stored; caps the travel speed |
| `name:en` → `name`, plus `ref` | road label, e.g. "Bengaluru Highway (NH 44)" |

**Not yet applied:** turn restrictions (`type=restriction` relations), time
conditional access, ferries. The import report says so.

## Travel-time model

Baseline speed (km/h) by class — planning assumptions for Indian roads, not
measurements:

| motorway | trunk | primary | secondary | tertiary | unclassified | residential | service | track |
|---|---|---|---|---|---|---|---|---|
| 80 | 70 | 55 | 45 | 35 | 25 | 25 | 15 | 12 |

`speed = min(class default × 1.25, maxspeed)` when `maxspeed` is tagged,
otherwise the class default (× 0.75 for links). `time_s = distance_m ÷ (speed ÷ 3.6)`.
These are honest *baseline estimates*; the API labels them that way unless
live traffic is configured.

## Place search index

Place nodes become rows in `places` with FTS5 (`unicode61`, diacritics
removed) over name, `name:en`, state and district. `state`/`district` come
from `addr:state`, `is_in:state`, `addr:district`, `is_in:district` when
present. Smaller places get `near` = nearest city/town within 50 km to help
tell same-named villages apart. Assembling full administrative boundaries
from OSM relations (for complete state/district on every village) is a
planned improvement.

## Memory and time

Measured on the real Monaco extract (1.7 MB PBF, 180 k nodes): 0.5 s total,
2,391 graph nodes / 4,559 edges, 92.0 % in the main component.

**All-India figures have not been measured here** (this development sandbox
cannot download Geofabrik data). Formulas for planning — measure with
`scripts/import_osm.sh`, which records peak RSS:

* importer peak ≈ 16 B × way-node references (+ sort copy) + 21 B × referenced nodes + final graph
* graph.bin / server graph ≈ 25 B × nodes + 4 B × nodes (CSR) + 48 B × directed edges + 8 B × shape points + names
* spatial index ≈ 12 B × (nodes + edge vertices)
* each route worker ≈ 37 B × nodes

For example, a graph of 30 M junction nodes and 70 M directed edges would
need roughly 0.9 + 3.4 GB for the graph, plus ~1.1 GB per route worker.
These are arithmetic estimates for planning, not benchmark results.
