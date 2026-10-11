#!/bin/sh
# Start the server with settings from ../.env (if present).
#   scripts/run.sh             sample network (synthetic) unless .env sets IRA_GRAPH_PATH
#   scripts/run.sh monaco      the real Monaco OSM test extract (imports it first if needed)
set -eu
export MSYS2_ARG_CONV_EXCL="*"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ -f "$ROOT/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$ROOT/.env"
    set +a
fi

if [ "${1:-}" = "monaco" ]; then
    mkdir -p "$ROOT/data/processed"
    if [ ! -f "$ROOT/data/processed/monaco.graph.bin" ]; then
        "$ROOT/backend/build/ira_import" --input "$ROOT/data/test_osm/monaco.osm.pbf" \
            --output "$ROOT/data/processed/monaco.graph.bin" --db "$ROOT/data/processed/monaco.sqlite"
    fi
    export IRA_GRAPH_PATH="data/processed/monaco.graph.bin"
    export IRA_DB_PATH="data/processed/monaco.sqlite"
fi

export IRA_FRONTEND_DIR="${IRA_FRONTEND_DIR:-frontend}"
export IRA_SAMPLE_DIR="${IRA_SAMPLE_DIR:-data/sample}"
export IRA_TILE_CACHE_DIR="${IRA_TILE_CACHE_DIR:-data/tile_cache}"
export IRA_GRAPH_PATH="${IRA_GRAPH_PATH:-data/processed/southern-zone.graph.bin}"
export IRA_DB_PATH="${IRA_DB_PATH:-data/processed/southern-zone.sqlite}"
export IRA_PORT="${IRA_PORT:-8080}"
export IRA_BIND="${IRA_BIND:-127.0.0.1}"

cd "$ROOT"
if [ ! -f backend/build/ira_server ] && [ ! -f backend/build/ira_server.exe ]; then
    make -C backend -j2 all
fi

echo "Open http://${IRA_BIND}:${IRA_PORT}/ in your browser (Ctrl+C to stop)"
if [ -x backend/build/ira_server.exe ]; then
    exec ./backend/build/ira_server.exe
else
    exec ./backend/build/ira_server
fi
