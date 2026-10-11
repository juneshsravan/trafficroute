# Measured performance

All numbers below were measured with `backend/build/bench_route` (`-O2`) on
the development machine: **2 vCPU Intel Xeon @ 2.10 GHz, 7 GB RAM, Linux**,
single-threaded, random source/target pairs inside the main component,
fastest-time mode. Dijkstra and A* returned identical costs on every query.

| Dataset | Nodes | Directed edges | Graph memory | Dijkstra p50 / p95 | A* p50 / p95 | Mean settled (D / A*) |
|---|---|---|---|---|---|---|
| Monaco OSM extract (real, 300 queries, mean 6.4 km) | 2,391 | 4,559 | 0.5 MB | 0.12 / 0.24 ms | 0.15 / 0.38 ms | 1,115 / 888 |
| Synthetic grid 1000×1000 (60 queries, mean 77 km) | 1,000,000 | 3,996,000 | 212 MB | 142 / 291 ms | 70 / 339 ms | 489,813 / 254,325 |
| Synthetic grid 2000×2000 (30 queries, mean 194 km) | 4,000,000 | 15,992,000 | 847 MB | 1,025 / 1,469 ms | 643 / 1,402 ms | 2,685,741 / 1,426,174 |

Reproduce:

```bash
cd backend
./build/bench_route --graph ../data/processed/monaco.graph.bin --queries 300 --seed 7
./build/bench_route --grid 1000 --queries 60 --seed 3
./build/bench_route --grid 2000 --queries 30 --seed 5
```

What this does and does not show

* The synthetic grids are **not road data**; they test scaling of the C
  engine on graphs of national-ish size with realistic edge lengths.
* A* settles roughly half the nodes of Dijkstra on long trips; on tiny
  graphs the haversine computation makes it slightly slower in wall time.
* Plain Dijkstra/A* on a multi-million-node graph takes on the order of a
  second for long routes on this machine. A full-India graph (tens of
  millions of nodes) will be slower for cross-country trips. The planned
  remedy is a speed-up technique computed at import time — **Contraction
  Hierarchies** or **ALT landmarks**, both implementable in C — and
  `IRA_MAX_SETTLED` bounds worst-case request time until then.
* No India-wide measurement has been made yet; run
  `scripts/import_osm.sh india` on a machine with enough RAM to get one.
