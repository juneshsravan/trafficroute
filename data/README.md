# Data

## `sample/` — synthetic Phase 1 test network

**This is not real road data.** It has 10 invented junctions placed near
17.00 N, 79.00 E only so distances are on a realistic scale for the A*
heuristic and for later map display.

```
          E ═══════ trunk (TOLL) ═══════ F
         /                                \
        A ───── B ───── C ───── D  ◄────────┘
                │\      │       ▲  ▲
                │ └─►H ─┼───────┘  │      B→H→D is one-way
                │       │          │
                G ──────┘          │      G–C is a restricted service road
                └──────────────────┘      G–D tertiary

        I ───── J    (separate component, unreachable from A–H)
```

| Edge | Road | Class | Speed | Notes |
|---|---|---|---|---|
| 1,2,3 | A-B, B-C, C-D | residential | 25 km/h | length = straight line (~2.13 km each) |
| 4,6 | A-E, F-D | trunk | 70 km/h | 3.0 km each |
| 5 | E-F | trunk | 70 km/h | 4.8 km, **toll** |
| 7 | B-G | tertiary | 35 km/h | 2.6 km |
| 8 | G-D | tertiary | 35 km/h | 4.0 km |
| 9,10 | B→H, H→D | secondary | 45 km/h | **one-way** |
| 11 | G-C | service | 15 km/h | 2.6 km, **restricted** |
| 12 | I-J | residential | 25 km/h | disconnected island |

### Hand-calculated expected routes A → D

time (s) = metres ÷ (km/h ÷ 3.6)

| Case | Route | Distance | Time |
|---|---|---|---|
| Shortest | A-B-C-D | 6 380 m | 6380 ÷ 6.944 = 919 s |
| Fastest | A-E-F-D | 10 800 m | 10800 ÷ 19.44 = 555 s |
| Fastest, avoid tolls | A-B-H-D | 2127 + 4916 = 7 044 m | 306 + 393 = 700 s |
| Fastest, E→F congested to 1200 s | A-B-H-D | 7 044 m | 700 s (vs 3000/19.44+1200+3000/19.44 = 1509 s) |
| Avoid tolls + B→H closed | A-B-C-D | 6 380 m | 919 s (beats A-B-G-D at 985 s) |
| D → A, avoid tolls | D-C-B-A | — | one-way H cannot be used backwards |
| A → I | none | — | `NO_PATH` |

These are exactly the cases asserted in `backend/tests/test_routing.c`.

## `sample/places.csv`

Synthetic place names for the sample network so location search works in
demo mode.

## `test_osm/`

* `monaco.osm.pbf` — **real** OpenStreetMap data (Monaco and surroundings,
  1.7 MB), copied from the OSRM project's test data. © OpenStreetMap
  contributors, ODbL. Used by `make test-import` and `scripts/run.sh monaco`.
* `rules_test.osm` — hand-written OSM XML with expected results in its header;
  exercises one-way, access, toll, footway and place rules
  (`tests/test_osm_rules.c`).

## Real data

Download from Geofabrik (ODbL 1.0, attribution "© OpenStreetMap contributors"):

* Regional first: `https://download.geofabrik.de/asia/india/southern-zone-latest.osm.pbf`
* All India (~1.6 GB as of Oct 2026): `https://download.geofabrik.de/asia/india-latest.osm.pbf`

Downloaded `.pbf` files and generated graph files are git-ignored and never
produced by a normal build.
