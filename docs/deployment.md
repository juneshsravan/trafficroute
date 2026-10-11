# Deployment

## Linux host (recommended)

```bash
sudo apt install build-essential pkg-config libmicrohttpd-dev libcurl4-openssl-dev \
                 libsqlite3-dev libreadosm-dev
scripts/build.sh                     # builds and runs the tests
scripts/import_osm.sh southern-zone  # or: india
cp .env.example .env                 # set IRA_GRAPH_PATH, IRA_DB_PATH, IRA_ADMIN_TOKEN
scripts/run.sh
```

### systemd unit

```ini
# /etc/systemd/system/indiaroute.service
[Unit]
Description=IndiaRoute AI routing server
After=network.target

[Service]
User=indiaroute
WorkingDirectory=/opt/indiaroute/backend
EnvironmentFile=/opt/indiaroute/.env
ExecStart=/opt/indiaroute/backend/build/ira_server
Restart=on-failure
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/opt/indiaroute/data/processed
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

### Reverse proxy (TLS)

Keep `IRA_BIND=127.0.0.1` and put nginx (or Caddy) in front for HTTPS:

```nginx
server {
    listen 443 ssl http2;
    server_name routes.example.in;
    # ssl_certificate ... (e.g. Let's Encrypt)
    client_max_body_size 64k;
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header X-Forwarded-For $remote_addr;
        proxy_read_timeout 30s;
    }
}
```
and set `IRA_TRUST_PROXY=1` so rate limits apply per real client.

## Windows

Build and run inside **WSL2 (Ubuntu)** with the commands above; open
`http://localhost:8080` in the Windows browser. MSYS2/MinGW-w64 also has the
libraries (`mingw-w64-ucrt-x86_64-libmicrohttpd`, `-curl`, `-sqlite3`) but the
server uses POSIX sockets/threads and readosm has no MSYS2 package, so WSL2 is
the supported path.

## Docker

`Dockerfile` builds the C binaries in an Ubuntu 24.04 stage (running the tests)
and copies them into a slim runtime image as a non-root user. Mount imported
data at `/data`. *The Dockerfile was written but could not be built in the
development sandbox (no Docker daemon); build and test it once on your host.*

## Updating the road graph

1. Run `ira_import` to a **new** file (e.g. `india-2026-11-01.graph.bin`).
2. Check the report (`*.report.json`) and run `bench_route --graph` on it.
3. Point `IRA_GRAPH_PATH` at it and restart. Writes are atomic (`.tmp` +
   rename), so a half-written graph is never loaded; the loader validates the
   checksum and every offset.
4. Persisted incidents are re-attached to the new graph by coordinate.

## Backups

`graph.bin` is reproducible from the PBF. Back up the SQLite file (incident
history, traffic audit log): `sqlite3 india.sqlite ".backup backup.sqlite"`
works while the server runs (WAL mode).

## Map tiles

`IRA_TILE_URL` defaults to `tile.openstreetmap.org`, whose usage policy allows
light use only and forbids heavy/production traffic. For production use a
self-hosted tile server or a commercial provider and set `IRA_TILE_URL` /
`IRA_TILE_ATTRIBUTION`. The page's Content-Security-Policy allows images from
any `https:` origin, so no code change is needed.

## External services needing keys or contracts

| Service | Needed for | Status |
|---|---|---|
| Geofabrik downloads | road data | free, no key; ODbL attribution |
| Tile server | map background | OSM default for development only |
| Live traffic provider | real-time traffic | none configured; needs a provider contract + adapter to the normalised feed |

## Monitoring

* `GET /api/v1/health` — status, uptime, request counters, mean route time,
  provider last success/error. Returns `"degraded"` when a configured traffic
  feed has not succeeded for 3 poll intervals.
* Logs go to stderr (journald): one line per API request with status and
  duration; never headers, bodies or tokens.
