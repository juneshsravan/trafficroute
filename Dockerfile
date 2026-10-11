# IndiaRoute AI — C server image. Build:  docker build -t indiaroute .
# Run (sample):  docker run -p 8080:8080 indiaroute
# Run (imported data mounted at /data):
#   docker run -p 8080:8080 -v $PWD/data/processed:/data \
#     -e IRA_GRAPH_PATH=/data/southern-zone.graph.bin -e IRA_DB_PATH=/data/southern-zone.sqlite indiaroute
FROM ubuntu:24.04 AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential pkg-config libmicrohttpd-dev libcurl4-openssl-dev libsqlite3-dev libreadosm-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY backend ./backend
COPY data/sample ./data/sample
COPY data/test_osm ./data/test_osm
RUN cd backend && make -j"$(nproc)" all && make test

FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libmicrohttpd12t64 libcurl4t64 libsqlite3-0 libreadosm1 ca-certificates \
    && rm -rf /var/lib/apt/lists/* && useradd --system --uid 10001 indiaroute
COPY --from=build /src/backend/build/ira_server /src/backend/build/ira_import /usr/local/bin/
COPY frontend /srv/frontend
COPY data/sample /srv/sample
ENV IRA_BIND=0.0.0.0 IRA_PORT=8080 IRA_FRONTEND_DIR=/srv/frontend IRA_SAMPLE_DIR=/srv/sample
USER indiaroute
EXPOSE 8080
HEALTHCHECK CMD ["/bin/bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080 || exit 1"]
CMD ["ira_server"]
