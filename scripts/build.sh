#!/bin/sh
# Build all C binaries and run the test suites.
#   scripts/build.sh            build + unit/API tests
#   scripts/build.sh --full     also sanitizers and the real-OSM importer test
set -eu
cd "$(dirname "$0")/../backend"

missing=""
for h in microhttpd.h sqlite3.h readosm.h; do
    echo "#include <$h>" | ${CC:-cc} -E -x c - >/dev/null 2>&1 || missing="$missing $h"
done
echo "#include <curl/curl.h>" | ${CC:-cc} -E -x c - >/dev/null 2>&1 || missing="$missing curl/curl.h"
if [ -n "$missing" ]; then
    echo "Missing development headers:$missing"
    echo "Install them with:"
    echo "  sudo apt install build-essential pkg-config libmicrohttpd-dev libcurl4-openssl-dev libsqlite3-dev libreadosm-dev"
    exit 1
fi

make -j"$(nproc 2>/dev/null || echo 2)" all
make test
if [ "${1:-}" = "--full" ]; then
    make test-import
    make sanitize
    make clean && make -j2 all
fi
echo "Build OK: backend/build/{ira_server,ira_import,route_cli,bench_route}"
