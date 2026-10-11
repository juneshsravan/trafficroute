#!/bin/sh
# Download an OpenStreetMap extract from Geofabrik and import it.
#   scripts/import_osm.sh southern-zone     (recommended first: South India)
#   scripts/import_osm.sh india             (whole country, ~1.6 GB download)
# Zones: northern-zone southern-zone eastern-zone western-zone central-zone north-eastern-zone
# Data licence: ODbL 1.0 - attribution "(c) OpenStreetMap contributors" required.
set -eu
REGION="${1:-southern-zone}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/data/processed"
mkdir -p "$OUT"

case "$REGION" in
    india) URL="https://download.geofabrik.de/asia/india-latest.osm.pbf" ;;
    northern-zone|southern-zone|eastern-zone|western-zone|central-zone|north-eastern-zone)
        URL="https://download.geofabrik.de/asia/india/${REGION}-latest.osm.pbf" ;;
    *) echo "unknown region: $REGION"; exit 2 ;;
esac

PBF="$OUT/$REGION-latest.osm.pbf"
if [ ! -f "$PBF" ]; then
    echo "Downloading $URL"
    curl -fL --retry 3 -o "$PBF.part" "$URL"
    mv "$PBF.part" "$PBF"
fi

cd "$ROOT/backend"
[ -x build/ira_import ] || make -j2 all
/usr/bin/env time -v ./build/ira_import --input "$PBF" --output "$OUT/$REGION.graph.bin" \
    --db "$OUT/$REGION.sqlite" --verbose 2> "$OUT/$REGION.import.log" || \
    ./build/ira_import --input "$PBF" --output "$OUT/$REGION.graph.bin" --db "$OUT/$REGION.sqlite" --verbose
grep -E "Maximum resident|Elapsed" "$OUT/$REGION.import.log" 2>/dev/null || true

echo
echo "Measuring route latency on the imported graph:"
./build/bench_route --graph "$OUT/$REGION.graph.bin" --queries 100

echo
echo "Start the server with:"
echo "  IRA_GRAPH_PATH=$OUT/$REGION.graph.bin IRA_DB_PATH=$OUT/$REGION.sqlite scripts/run.sh"
