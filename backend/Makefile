# IndiaRoute AI — backend build (C11 only; no C++ anywhere).
#
#   make              build everything: ira_server, ira_import, route_cli, bench_route
#   make test         unit + API tests (no network, no external data)
#   make test-import  importer test on the real OSM extract in data/test_osm
#   make sanitize     all tests under AddressSanitizer + UndefinedBehaviorSanitizer
#   make valgrind     all tests under valgrind (run on a non-sanitizer build)
#   make clean
#
# Dependencies (Debian/Ubuntu):
#   sudo apt install build-essential pkg-config libmicrohttpd-dev \
#        libcurl4-openssl-dev libsqlite3-dev libreadosm-dev

CC      ?= cc
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion \
           -Wstrict-prototypes -Wmissing-prototypes -Werror
OPT     ?= -O2 -g
INC     := -Iinclude -Ithird_party/cjson -Iimporter
CFLAGS  += $(STD) -D_POSIX_C_SOURCE=200809L $(WARN) $(OPT) $(INC)

PKGS        := libmicrohttpd libcurl sqlite3
PKG_CFLAGS  := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS    := $(shell pkg-config --libs $(PKGS) 2>/dev/null || echo -lmicrohttpd -lcurl -lsqlite3)
LIBS_CORE   := -lm
LIBS_SERVER := $(PKG_LIBS) -lpthread -lm
ifeq ($(OS),Windows_NT)
LIBS_SERVER += -lws2_32
endif
LIBS_IMPORT := -lreadosm -lsqlite3 -lpthread -lm

BUILD := build

CORE_SRC   := src/graph.c src/heap.c src/geospatial.c src/routing.c src/dijkstra.c \
              src/astar.c src/graph_io.c src/graph_bin.c src/spatial_index.c \
              src/route_geometry.c src/route_alternatives.c src/traffic.c
SERVER_SRC := src/config.c src/log.c src/rate_limit.c src/database.c src/tile_cache.c \
              src/location_search.c src/traffic_feed.c src/app.c src/api.c src/http_server.c
IMPORT_SRC := importer/osm_parser.c importer/graph_builder.c importer/import_validation.c

CORE_O   := $(CORE_SRC:%.c=$(BUILD)/%.o)
SERVER_O := $(SERVER_SRC:%.c=$(BUILD)/%.o)
IMPORT_O := $(IMPORT_SRC:%.c=$(BUILD)/%.o)
CJSON_O  := $(BUILD)/third_party/cjson/cJSON.o

CORE_TESTS   := test_heap test_graph test_dijkstra test_routing test_spatial \
                test_graph_bin test_alternatives test_geometry test_traffic
SERVER_TESTS := test_api test_feed test_rate_limit
IMPORT_TESTS := test_osm_rules
ALL_TESTS    := $(CORE_TESTS) $(SERVER_TESTS) $(IMPORT_TESTS)
TEST_BINS    := $(ALL_TESTS:%=$(BUILD)/%)

BINS := $(BUILD)/ira_server $(BUILD)/ira_import $(BUILD)/route_cli $(BUILD)/bench_route

.PHONY: all test test-import sanitize valgrind clean
all: $(BINS)

HEADERS := $(wildcard include/*.h importer/*.h)

$(BUILD)/src/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

$(BUILD)/importer/%.o: importer/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Vendored third-party code: compiled as C11 but without our -Werror policy.
$(CJSON_O): third_party/cjson/cJSON.c third_party/cjson/cJSON.h
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(OPT) -w -c $< -o $@

$(BUILD)/ira_server: $(BUILD)/src/main.o $(SERVER_O) $(CORE_O) $(CJSON_O)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_SERVER)

$(BUILD)/ira_import: $(BUILD)/importer/main.o $(IMPORT_O) $(BUILD)/src/database.o $(CORE_O) $(CJSON_O)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_IMPORT)

$(BUILD)/route_cli: $(BUILD)/src/route_cli.o $(CORE_O)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_CORE)

$(BUILD)/bench_route: $(BUILD)/src/bench_route.o $(CORE_O)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_CORE)

# ---- tests ----
$(CORE_TESTS:%=$(BUILD)/%): $(BUILD)/%: tests/%.c tests/test_util.h $(CORE_O)
	$(CC) $(CFLAGS) -Itests $< $(CORE_O) -o $@ $(LIBS_CORE)

$(SERVER_TESTS:%=$(BUILD)/%): $(BUILD)/%: tests/%.c tests/test_util.h $(SERVER_O) $(CORE_O) $(CJSON_O)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Itests $< $(SERVER_O) $(CORE_O) $(CJSON_O) -o $@ $(LIBS_SERVER)

$(BUILD)/test_osm_rules: tests/test_osm_rules.c tests/test_util.h $(IMPORT_O) $(CORE_O)
	$(CC) $(CFLAGS) -Itests $< $(IMPORT_O) $(CORE_O) -o $@ $(LIBS_IMPORT)

test: $(TEST_BINS) $(BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done
	@echo "All test suites passed."

test-import: $(BINS)
	@sh tests/test_import.sh

sanitize:
	$(MAKE) clean
	$(MAKE) test OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

valgrind: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do \
	  valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,indirect -q ./$$t > /dev/null; \
	  echo "valgrind clean: $$t"; done

clean:
	rm -rf $(BUILD)
