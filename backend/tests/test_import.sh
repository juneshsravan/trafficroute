#!/bin/sh
# Importer test on REAL OpenStreetMap data: the Monaco extract used by the
# OSRM project's test suite (data/test_osm/monaco.osm.pbf, (c) OpenStreetMap
# contributors, ODbL). Checks the import report, then routes 300 random
# pairs inside the main component with Dijkstra and A* (bench_route exits
# non-zero on any failed route or cost mismatch), then starts the server and
# requests a route over HTTP.
set -eu
cd "$(dirname "$0")/.."
OUT=build/import_test
mkdir -p "$OUT"
PBF=../data/test_osm/monaco.osm.pbf

echo "== import $PBF"
./build/ira_import --input "$PBF" --output "$OUT/monaco.graph.bin" --db "$OUT/monaco.sqlite" \
    --report "$OUT/report.json" > "$OUT/import.log"
cat "$OUT/import.log"

fail() { echo "FAIL: $*"; exit 1; }
json_num() { grep "\"$1\"" "$OUT/report.json" | head -1 | sed 's/[^0-9.]*\([0-9.]*\).*/\1/'; }

nodes=$(json_num nodes)
edges=$(json_num directed_edges)
share=$(json_num largest_component_share)
places=$(json_num places)
[ "$nodes" -gt 1000 ] || fail "too few nodes ($nodes)"
[ "$edges" -gt 2000 ] || fail "too few edges ($edges)"
[ "$places" -gt 10 ] || fail "too few places ($places)"
awk "BEGIN{exit !($share > 0.85)}" || fail "main component share $share"
echo "report ok: $nodes nodes, $edges edges, main component share $share, $places places"

echo "== random routes on the imported graph"
./build/bench_route --graph "$OUT/monaco.graph.bin" --queries 300 --seed 7

echo "== server check + HTTP route"
PORT=18093
IRA_GRAPH_PATH="$OUT/monaco.graph.bin" IRA_DB_PATH="$OUT/monaco.sqlite" ./build/ira_server --check
IRA_GRAPH_PATH="$OUT/monaco.graph.bin" IRA_DB_PATH="$OUT/monaco.sqlite" IRA_PORT=$PORT IRA_LOG_LEVEL=2 \
    ./build/ira_server > "$OUT/server.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
for i in 1 2 3 4 5 6 7 8 9 10; do
    curl -sf "http://127.0.0.1:$PORT/api/v1/health" > /dev/null 2>&1 && break
    sleep 0.3
done
resp=$(curl -sf -X POST "http://127.0.0.1:$PORT/api/v1/routes" -H 'Content-Type: application/json' \
    -d '{"origin":{"latitude":43.7310,"longitude":7.4248},"destination":{"latitude":43.7500,"longitude":7.4400},"alternatives":1}')
echo "$resp" | grep -q '"status":"success"' || fail "HTTP route failed: $resp"
echo "$resp" | grep -q 'Boulevard' || fail "expected named Monaco roads in instructions"
search=$(curl -sf "http://127.0.0.1:$PORT/api/v1/locations/search?q=Monaco")
echo "$search" | grep -q '"routable":true' || fail "place search failed: $search"
echo "HTTP route + place search ok"
echo "Importer test passed."
