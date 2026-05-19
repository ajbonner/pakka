# Requires GNU make. On BSD: pkg_add gmake (or pkg install gmake), then run gmake.
APP_NAME="\"Pakka\""
MAJOR=1
MINOR=3
PATCH=0
BUILD_DATE="\"$(shell date +'%b %d, %Y')\""
VERSION="\"$(MAJOR).$(MINOR).$(PATCH)\""
TARGET=pakka
# _XOPEN_SOURCE=700 (POSIX.1-2008 + XSI) — needed for realpath():
# glibc gates it on __USE_XOPEN_EXTENDED, musl on _XOPEN_SOURCE, neither
# on _POSIX_C_SOURCE.
CPPFLAGS = -Iinclude -D_XOPEN_SOURCE=700 -D_DEBUG=1 -DAPP_NAME=$(APP_NAME) -DVERSION=$(VERSION) -DBUILD_DATE=$(BUILD_DATE)
CC=cc $(CPPFLAGS)
CFLAGS=-g -Wall --std=c99 --pedantic
AR ?= ar

SRC_DIR=src
INCLUDE_DIR=include
BUILD_DIR=build
OBJ_DIR=$(BUILD_DIR)/obj
LIB_DIR=$(BUILD_DIR)/lib
TEST_DIR=$(BUILD_DIR)/test

SOURCES=$(wildcard $(SRC_DIR)/*.c)
# Vendored sources live under src/vendor/. Kept out of $(SOURCES) so
# the lint target (clang-tidy WarningsAsErrors:'*') skips them — patches
# against upstream are out of scope. The objects still land in
# libpakka.a via LIB_OBJECTS.
VENDOR_SOURCES=$(wildcard $(SRC_DIR)/vendor/puff/*.c)
OBJECTS=$(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Library is every src/*.c except cli.c (CLI-only) plus the vendored
# objects. `symbol-audit` below enforces only pakka_*-prefixed names
# leave the archive — vendored puff was renamed `puff` -> `pakka_inflate`
# at vendor time so it passes the same gate.
LIB_SOURCES=$(filter-out $(SRC_DIR)/cli.c,$(SOURCES))
LIB_OBJECTS=$(LIB_SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o) \
            $(VENDOR_SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CLI_OBJECTS=$(OBJ_DIR)/cli.o
LIBPAKKA=$(LIB_DIR)/libpakka.a

QUAKE_URL=https://www.libsdl.org/projects/quake/data/quakesw-1.0.6.tar.gz
QUAKE_SHA256=d173e9f828b932a8160d4c65927281d0c28131cd922f0bf0d69e92a35185b499
QUAKE_TARBALL=$(TEST_DIR)/quakesw.tar.gz
PAK0=$(TEST_DIR)/pak0.pak

# Q3 demo wrapper from archive.org (redistribution of id's freely
# distributable demo). Used by the optional `slow-test` target only —
# not pulled by default `make test`. archive.org's CDN is occasionally
# 503; if the fetch fails, slow-test fails but normal CI is unaffected.
Q3DEMO_URL=https://archive.org/download/Q3A-Demo/Quake%203%20Arena%20Demo.zip
Q3DEMO_SHA256=e9f89ef064317634aab3b3a3add131887967fc04744526bd624e1914b1e25b3e
Q3DEMO_ZIP=$(TEST_DIR)/q3demo.zip
Q3DEMO_PAK0_PK3=$(TEST_DIR)/q3demo/pak0.pk3

CLANG_TIDY ?= clang-tidy
NM ?= nm

# Public header. Linted explicitly via lint-header so transitive-include
# regressions in include/pakka.h surface even when every internal TU
# happens to pull in the missing dependency for unrelated reasons.
PUBLIC_HEADERS = $(INCLUDE_DIR)/pakka.h

.PHONY: all clean test test-clean distclean lint lint-header symbol-audit c_api_test verify-tarball verify-q3demo fixture slow-test

all: $(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(LIB_DIR) $(TARGET)

test-clean:
	rm -rf $(TEST_DIR)/extracted $(TEST_DIR)/re_extracted $(TEST_DIR)/rebuilt.pak $(TEST_DIR)/crud $(TEST_DIR)/id1

distclean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Verify the public header parses as standalone C with the same warning
# flags the rest of the codebase uses. Catches regressions where the
# header silently relies on transitive includes from an internal TU.
lint-header:
	$(CC) $(CFLAGS) -fsyntax-only -x c $(PUBLIC_HEADERS)

lint: lint-header
	$(CLANG_TIDY) --quiet $(SOURCES) $(PUBLIC_HEADERS) -- $(CPPFLAGS) --std=c99

# Hard gate against non-pakka_ defined globals leaking out of
# libpakka.a. Prints the full list AND exits non-zero on any violation;
# `make test` depends on this. Filters NF==3 to skip nm's "filename:"
# lines and 2-field undefined refs. Includes 'C' (tentative/common)
# alongside T/D/B/R/S so an uninitialized global can't sneak past.
# Skips names containing '.' — those are compiler-emitted artifacts
# like gcc's i386 PIC thunks _x86.get_pc_thunk.{ax,bx,si}; legitimate
# C identifiers can't contain a dot, so this can't hide a pakka leak.
symbol-audit: $(LIBPAKKA)
	@all=`$(NM) -g $(LIBPAKKA) | awk 'NF == 3 && $$2 ~ /^[TDBRSC]$$/ { sub(/^_/, "", $$3); if ($$3 ~ /\./) next; print $$3 }' | sort -u`; \
	bad=`printf '%s\n' "$$all" | grep -v '^pakka_' || true`; \
	printf '%s\n' "$$all"; \
	if [ -n "$$bad" ]; then \
		echo "" >&2; \
		echo "symbol-audit FAILED: non-pakka_ defined globals in $(LIBPAKKA):" >&2; \
		printf '%s\n' "$$bad" >&2; \
		exit 1; \
	fi

$(TARGET): $(CLI_OBJECTS) $(LIBPAKKA)
	$(CC) $(CFLAGS) -o $(TARGET) $(CLI_OBJECTS) $(LIBPAKKA)

# ar rcs: r = insert/replace, c = create silently, s = write index.
# Inline mkdir keeps us GNU make 3.79.1 compatible (no order-only prereqs).
$(LIBPAKKA): $(LIB_OBJECTS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $(LIB_OBJECTS)

# C-API test binary. Built against the public include/ surface only,
# linked against the static archive. Exercises every pakka_* public
# function: NULL tolerance, structured-error population, opaque-entry
# accessors, the streaming reader, add/delete/commit round-trips, and
# the verify report callback — none of which the bats CLI suite can
# reach.
C_API_TEST = $(TEST_DIR)/c_api_test
$(C_API_TEST): tests/c_api_test.c $(LIBPAKKA)
	@mkdir -p $(TEST_DIR)
	$(CC) $(CFLAGS) -o $@ tests/c_api_test.c $(LIBPAKKA)
c_api_test: $(C_API_TEST)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Order-only prereqs (`|`) arrived in GNU make 3.80; 3.79.1 errors on
# them, so we keep the directory creation inline.
$(QUAKE_TARBALL):
	@mkdir -p $(TEST_DIR)
	@echo "==> Downloading Quake shareware ($(QUAKE_URL))"
	@curl -fsSL -o $(QUAKE_TARBALL) $(QUAKE_URL)

# Phony so the SHA check runs every build, including cache-restore hits
# where $(QUAKE_TARBALL) is already on disk and would otherwise be skipped.
verify-tarball: $(QUAKE_TARBALL)
	@actual=`openssl dgst -sha256 $(QUAKE_TARBALL) | awk '{print $$NF}'`; \
	if [ "$$actual" != "$(QUAKE_SHA256)" ]; then \
		echo "==> SHA256 mismatch: expected $(QUAKE_SHA256), got $$actual"; \
		rm -f $(QUAKE_TARBALL); \
		exit 1; \
	fi

$(PAK0): verify-tarball
	@cd $(TEST_DIR) && tar xzf quakesw.tar.gz
	@cp $(TEST_DIR)/id1/pak0.pak $(PAK0)

# Download + SHA-verify pak0.pak only. Useful for CI configurations that
# build pakka through a non-Make path (e.g. CMake/MSVC on Windows) but
# still want to drive the bats suite against the canonical fixture.
fixture: $(PAK0)

test: $(TARGET) $(PAK0) $(C_API_TEST) symbol-audit
	CFLAGS='$(CFLAGS)' bats tests/

# Q3 demo wrapper download + SHA verify. archive.org gives SHA1; we
# re-compute SHA256 once at vendor time and pin that here.
$(Q3DEMO_ZIP):
	@mkdir -p $(TEST_DIR)
	@echo "==> Downloading Q3 demo wrapper ($(Q3DEMO_URL))"
	@curl -fsSL -o $(Q3DEMO_ZIP) "$(Q3DEMO_URL)"

verify-q3demo: $(Q3DEMO_ZIP)
	@actual=`openssl dgst -sha256 $(Q3DEMO_ZIP) | awk '{print $$NF}'`; \
	if [ "$$actual" != "$(Q3DEMO_SHA256)" ]; then \
		echo "==> SHA256 mismatch on Q3 demo: expected $(Q3DEMO_SHA256), got $$actual"; \
		rm -f $(Q3DEMO_ZIP); \
		exit 1; \
	fi

# Extract the inner pak0.pk3 from the wrapper using pakka itself —
# tests pakka's PK3 reader against a real id-made archive on the way
# to producing the fixture.
$(Q3DEMO_PAK0_PK3): $(TARGET) verify-q3demo
	@mkdir -p $(TEST_DIR)/q3demo_raw
	./$(TARGET) -xf $(Q3DEMO_ZIP) -C $(TEST_DIR)/q3demo_raw >/dev/null
	@mkdir -p $(TEST_DIR)/q3demo
	@cp $(TEST_DIR)/q3demo_raw/Quake\ 3\ Arena\ Demo/demoq3/pak0.pk3 $(Q3DEMO_PAK0_PK3)
	@echo "==> Q3 demo pak0.pk3 ready: $(Q3DEMO_PAK0_PK3)"

# Optional: full PK3 suite against id's real Q3 demo pak0.pk3. Pulls
# 93 MiB from archive.org, so kept out of `make test`. CI can opt in
# by running `make slow-test` on a single representative job.
slow-test: $(TARGET) $(Q3DEMO_PAK0_PK3) symbol-audit
	Q3DEMO_PAK0_PK3=$(abspath $(Q3DEMO_PAK0_PK3)) bats tests/pk3_q3demo.bats
