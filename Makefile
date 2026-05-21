# Requires GNU make. On BSD: pkg_add gmake (or pkg install gmake), then run gmake.
APP_NAME="\"Pakka\""
MAJOR=1
MINOR=4
PATCH=0
BUILD_DATE="\"$(shell date +'%b %d, %Y')\""
VERSION="\"$(MAJOR).$(MINOR).$(PATCH)\""
TARGET=pakka
# _XOPEN_SOURCE=700 (POSIX.1-2008 + XSI) — needed for realpath():
# glibc gates it on __USE_XOPEN_EXTENDED, musl on _XOPEN_SOURCE, neither
# on _POSIX_C_SOURCE.
# _FILE_OFFSET_BITS=64 — widens off_t / fseeko / ftello on 32-bit glibc
# so pakka_platform_fseek/ftell can address the [2 GiB, 4 GiB) range.
CPPFLAGS = -Iinclude -Isrc -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -D_DEBUG=1 -DAPP_NAME=$(APP_NAME) -DVERSION=$(VERSION) -DBUILD_DATE=$(BUILD_DATE)

# Opt-in fault-injection hook (PAKKA_INJECT_FAULT_AT="op:N" env var,
# read by pakka_test_should_fault in src/platform.c). Compiled into
# binaries only when the user invokes a test target, so a plain
# `make` produces a release-style build without the hook. The bats
# suite (`make test`) gets it automatically; the c_api_test exerciser
# and inline cc invocations link against the test-built libpakka.a
# and pick up the hook through it. CMake / Windows release builds
# never define PAKKA_TEST_BUILD.
ifneq ($(filter test test-fault slow-test, $(MAKECMDGOALS)),)
CPPFLAGS += -DPAKKA_TEST_BUILD
PAKKA_BUILD_MODE := test
else
PAKKA_BUILD_MODE := prod
endif
CC=cc $(CPPFLAGS)
CFLAGS=-g -Wall --std=c99 --pedantic
AR ?= ar

SRC_DIR=src
INCLUDE_DIR=include
BUILD_DIR=build
# Per-build-mode object + library directories. `make` and `make test`
# compile with different CPPFLAGS (the latter adds -DPAKKA_TEST_BUILD),
# so sharing OBJ/LIB lets the wrong-flavor .o or .a be reused after a
# mode switch — especially painful at second-resolution mtime
# comparisons where back-to-back builds collide. Splitting per mode
# keeps each cache warm and avoids the ambiguity. The bats suite picks
# up the right libpakka.a via the $LIBPAKKA env var the test target
# passes through.
OBJ_DIR=$(BUILD_DIR)/obj-$(PAKKA_BUILD_MODE)
LIB_DIR=$(BUILD_DIR)/lib-$(PAKKA_BUILD_MODE)
TEST_DIR=$(BUILD_DIR)/test

SOURCES=$(wildcard $(SRC_DIR)/*.c)

# DEFLATE backend selection. Default: bundled sdefl + sinfl single-
# header codecs (no external dependency). Opt-in: PAKKA_DEFLATE_BACKEND=zlib
# routes through the host's libz instead, useful for integrators (game
# engines) that already link zlib and don't want to carry the ~1400
# LOC of bundled codec code in their binary. Only one backend is
# compiled per build — the deflate impl files live under src/deflate/
# which the top-level wildcard above doesn't reach, so the conditional
# below is the only way they enter the source list.
PAKKA_DEFLATE_BACKEND ?= vendored
# Two-level if/else rather than `else ifeq` — GNU make 3.79.1 (the
# legacy floor; default on Red Hat 9 / Sarge) errors out on the
# combined form. The nested shape works on every supported make
# version from 3.79.1 forward.
ifeq ($(PAKKA_DEFLATE_BACKEND),zlib)
    DEFLATE_SOURCES=$(SRC_DIR)/deflate/deflate_zlib.c
    LDLIBS += -lz
else
ifeq ($(PAKKA_DEFLATE_BACKEND),vendored)
    DEFLATE_SOURCES=$(SRC_DIR)/deflate/deflate_vendored.c
else
    $(error PAKKA_DEFLATE_BACKEND must be 'vendored' or 'zlib', got '$(PAKKA_DEFLATE_BACKEND)')
endif
endif

# Vendored sources live under src/vendor/. Kept out of $(SOURCES) so
# the lint target (clang-tidy WarningsAsErrors:'*') skips them — patches
# against upstream are out of scope. The objects still land in
# libpakka.a via LIB_OBJECTS. The glob stays narrow on purpose: a
# broader src/vendor/*/*.c pattern would unconditionally pull in
# src/vendor/wingetopt/getopt.c + src/vendor/dirent/*.c, which are
# Windows-only shims gated under WIN32 in CMakeLists.txt and not
# linkable on Unix.
ifeq ($(PAKKA_DEFLATE_BACKEND),vendored)
VENDOR_SOURCES=$(wildcard $(SRC_DIR)/vendor/sdefl/*.c) \
               $(wildcard $(SRC_DIR)/vendor/sinfl/*.c)
else
VENDOR_SOURCES=
endif

OBJECTS=$(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o) \
        $(DEFLATE_SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Library is every src/*.c except cli.c (CLI-only) plus the selected
# deflate backend impl plus the vendored objects. `symbol-audit` below
# enforces only pakka_*-prefixed names leave the archive — both sdefl
# and sinfl were renamed `sdefl_`/`sinfl_` -> `pakka_sdefl_`/`pakka_sinfl_`
# at vendor time so they pass the same gate. The zlib backend links
# against host libz at executable-link time; libz's own symbols appear
# only as undefined references (U flag) in libpakka.a and don't
# violate the pakka_*-prefix rule.
LIB_SOURCES=$(filter-out $(SRC_DIR)/cli.c,$(SOURCES)) $(DEFLATE_SOURCES)
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

# GoldSrc PAK fixtures — two real Valve-built PACK archives from the
# Half-Life 1 lineage. GoldSrc PAK is bit-identical to Quake/Q2 PAK
# (same 12-byte header, 56-byte names, 64-byte entries), so these are
# parity-confirmation fixtures against real Valve writer output, not a
# separate code path. Driven by `slow-test-goldsrc` (separate from the
# Q3 demo's `slow-test`); default `make test` leaves them untouched
# and tests/pak_goldsrc.bats skips when the env vars are unset.
#
# Both wrappers are plain ZIPs from archive.org; we unzip then copy
# the inner pak0 out to a canonical path under build/test/. unzip is
# in every base Linux/BSD/macOS install plus MSYS2 — no extra
# dependency vs. the 7z/unrar paths the upstream archive uploaders
# could have chosen.
#
# Fixture A: Half-Life Uplink (1999 free standalone demo Valve made
# public for the launch promotion; the executable was distributed
# free of charge). 48 MiB zip from the half-life-day-one archive.org
# item (a community upload — pakka does not assert authorship; the
# SHA pin and the runtime-only fetch keep us out of bundling them).
# Inner path Half-LifeUplink/valve/pak0.PAK (uppercase extension, as
# the original shipped).
# Fixture B: Half-Life: Day One (1998 OEM video-card-bundled demo
# that came with retail Voodoo cards of the period). 90 MiB zip from
# the same archive.org item. Inner path Half-Life Day One/valve/pak0.pak.
# Both fixtures are fetched at test time only; the SHA pin ensures
# we run against the exact bytes we tested against. Neither file is
# committed to this repository.
GOLDSRC_UPLINK_URL=https://archive.org/download/half-life-day-one/Half-LifeUplink.zip
GOLDSRC_UPLINK_SHA256=6e06a9f25d36ec12750da8f94af24a71f26af2330a665a9d4922421db4459aa4
GOLDSRC_UPLINK_WRAPPER=$(TEST_DIR)/hl-uplink.zip
GOLDSRC_UPLINK_PAK0=$(TEST_DIR)/hl-uplink/valve/pak0.pak

GOLDSRC_DAYONE_URL=https://archive.org/download/half-life-day-one/Half-Life%20Day%20One.zip
GOLDSRC_DAYONE_SHA256=35098523a078cd2cde858a261e7071f1e1e79ae0c38fac9302186d3cd9bd001d
GOLDSRC_DAYONE_WRAPPER=$(TEST_DIR)/hl-dayone.zip
GOLDSRC_DAYONE_PAK0=$(TEST_DIR)/hl-dayone/valve/pak0.pak

CLANG_TIDY ?= clang-tidy
NM ?= nm

# Public header. Linted explicitly via lint-header so transitive-include
# regressions in include/pakka.h surface even when every internal TU
# happens to pull in the missing dependency for unrelated reasons.
PUBLIC_HEADERS = $(INCLUDE_DIR)/pakka.h

.PHONY: all clean test test-clean distclean lint lint-header lint-advisory lint-win32 coverage fuzz fuzz-open fuzz-dk fuzz-roundtrip symbol-audit c_api_test dk_codec_test verify-tarball verify-q3demo verify-goldsrc-uplink verify-goldsrc-dayone fixture slow-test slow-test-goldsrc

# Force serial execution. force-relink (below) deletes $(TARGET) and
# $(LIBPAKKA) as a sibling prereq of `all` / `test`; under `make -j`
# that races with the link rules and the c_api_test / dk_codec_test
# recipes that read $(LIBPAKKA). pakka is small enough that serial
# build is under a few seconds — the parallel-make speedup wouldn't
# be worth the race fragility.
.NOTPARALLEL:

all: force-relink $(TARGET)

# Always rebuild the top-level binary and per-mode libpakka.a. GNU
# Make 3.81 (Apple's default) compares mtimes at second resolution,
# so back-to-back `make` ↔ `make test` invocations can land in the
# same second and leave the wrong-flavor binary in place. The
# per-mode .o cache still survives (build/obj-prod/ vs obj-test/), so
# this only adds a relink — a fraction of a second.
.PHONY: force-relink
force-relink:
	@rm -f $(TARGET) $(LIBPAKKA)

clean:
	rm -rf $(BUILD_DIR)/obj-* $(BUILD_DIR)/lib-* $(BUILD_DIR)/fuzz $(BUILD_DIR)/coverage $(TARGET)

test-clean:
	rm -rf $(TEST_DIR)/extracted $(TEST_DIR)/re_extracted $(TEST_DIR)/rebuilt.pak $(TEST_DIR)/crud $(TEST_DIR)/id1

distclean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Verify the public header parses as standalone C with the same warning
# flags the rest of the codebase uses. Catches regressions where the
# header silently relies on transitive includes from an internal TU.
lint-header:
	$(CC) $(CFLAGS) -fsyntax-only -x c $(PUBLIC_HEADERS)

lint: lint-header lint-advisory
	$(CLANG_TIDY) --quiet $(SOURCES) $(PUBLIC_HEADERS) -- $(CPPFLAGS) --std=c99

# Cross-arm lint: run clang-tidy against the _WIN32 branch of every
# Win32-aware source. Linux's mingw-w64 headers stand in for the
# MSVC SDK so we can syntax-check the Windows arm without an MSVC
# toolchain. Catches regressions where someone touches src/platform.{h,c},
# src/cli.c's CLI path, or src/pakfile.c's PAKKA_ERR_DOMAIN_WIN32
# rebuild branches and ships a subtle break that only the
# windows-msvc job would catch. Targets src/cli.c + src/platform.c +
# src/pakfile.c — the three files with non-trivial _WIN32 code.
#
# Skips cleanly when the cross-headers are absent (e.g. on macOS
# without `brew install mingw-w64` — note Homebrew places them
# under $(brew --prefix mingw-w64), not /usr; override via
# `make WIN32_HEADERS=... lint-win32`). On Ubuntu the apt package
# `mingw-w64` drops them at the default path.
#
# CI sets REQUIRE_WIN32_HEADERS=1 to fail rather than skip when
# the headers are missing — installations there are deterministic
# and a missing-package case is a CI bug, not user laxness.
.PHONY: lint-win32
WIN32_HEADERS ?= /usr/x86_64-w64-mingw32/include
lint-win32:
	@if [ ! -d "$(WIN32_HEADERS)" ]; then \
		if [ "$(REQUIRE_WIN32_HEADERS)" = "1" ]; then \
			echo "lint-win32: Mingw cross-headers required but not found at $(WIN32_HEADERS)" >&2; \
			exit 1; \
		fi; \
		echo "lint-win32: Mingw cross-headers not found at $(WIN32_HEADERS)"; \
		echo "lint-win32: install via apt-get install mingw-w64 on Linux"; \
		echo "lint-win32: or brew install mingw-w64 on macOS"; \
		echo "lint-win32: (override path with WIN32_HEADERS=... if installed elsewhere); skipping"; \
		exit 0; \
	fi; \
	$(CLANG_TIDY) --quiet \
		--header-filter='(common|platform|filesystem|pakka)\.h$$' \
		src/cli.c src/platform.c src/pakfile.c $(PUBLIC_HEADERS) -- \
		$(CPPFLAGS) \
		--target=x86_64-w64-mingw32 \
		-isystem $(WIN32_HEADERS) \
		-Isrc -Isrc/vendor/wingetopt -Isrc/vendor/dirent \
		--std=c99 \
		-Wno-pragma-pack -Wno-pragma-system-header-outside-header

# Advisory lints — print warnings but don't fail the build. Today this
# is the add-path symmetry check (catches re-divergence of the H1
# hardening). Set STRICT=1 to make it fail on findings. Kept out of
# clang-tidy because the rule isn't easily expressed as a tidy check
# and we want it as a tripwire, not a gate.
.PHONY: lint-advisory
lint-advisory:
	@dev/lint/add-path-symmetry.sh $(CURDIR)

# Coverage report. Builds with -fprofile-arcs -ftest-coverage, runs
# the full bats suite, and renders an HTML lcov tree under
# build/coverage/. Needs lcov + genhtml; on macOS install via
# `brew install lcov`. Not wired into `make test` because the
# instrumented build adds ~30% runtime to bats and the report is only
# useful as a periodic artifact, not on every iteration.

# libFuzzer harnesses. Need clang with -fsanitize=fuzzer (Ubuntu's
# clang ships this by default; macOS needs `brew install llvm` +
# `CC=$(brew --prefix llvm)/bin/clang`). Built into build/fuzz/ with
# ASan as the bug oracle and the seed corpora living in dev/fuzz/.
# `make fuzz` builds all three; the per-target rules (fuzz-open,
# fuzz-dk, fuzz-roundtrip) build one harness each. CI runs each for
# 60s on push to master.
FUZZ_CC ?= clang
FUZZ_DIR := $(BUILD_DIR)/fuzz
FUZZ_CFLAGS := -g -O1 -Wall --std=c99 \
               -fsanitize=fuzzer,address,undefined \
               -fno-sanitize-recover=undefined \
               -fno-omit-frame-pointer

.PHONY: fuzz fuzz-open fuzz-dk fuzz-roundtrip
fuzz: fuzz-open fuzz-dk fuzz-roundtrip

# Each fuzzer compiles ALL library sources from scratch with the fuzz
# CFLAGS — libpakka-prod / libpakka-test were built without
# -fsanitize=fuzzer so we can't reuse them. The footprint is small
# (one .o file per source per harness) and the alternative is a
# third per-mode library that few people use.
# Compile flags shared by every fuzz harness. Mirrors the standard
# CPPFLAGS minus -DPAKKA_TEST_BUILD (no fault hooks needed) and minus
# -D_DEBUG so libFuzzer's stdout doesn't compete with library debug
# prints.
FUZZ_CPPFLAGS := -Iinclude -Isrc -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 \
                 -DAPP_NAME=$(APP_NAME) -DVERSION=$(VERSION) \
                 -DBUILD_DATE=$(BUILD_DATE)

$(FUZZ_DIR)/pakka_fuzz_open: dev/fuzz/pakka_fuzz_open.c $(LIB_SOURCES) $(VENDOR_SOURCES)
	@mkdir -p $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_CPPFLAGS) -o $@ $^

$(FUZZ_DIR)/pakka_fuzz_dk_inflate: dev/fuzz/pakka_fuzz_dk_inflate.c $(LIB_SOURCES) $(VENDOR_SOURCES)
	@mkdir -p $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_CPPFLAGS) -o $@ $^

$(FUZZ_DIR)/pakka_fuzz_roundtrip: dev/fuzz/pakka_fuzz_roundtrip.c $(LIB_SOURCES) $(VENDOR_SOURCES)
	@mkdir -p $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_CPPFLAGS) -o $@ $^

fuzz-open: $(FUZZ_DIR)/pakka_fuzz_open
fuzz-dk: $(FUZZ_DIR)/pakka_fuzz_dk_inflate
fuzz-roundtrip: $(FUZZ_DIR)/pakka_fuzz_roundtrip

.PHONY: coverage
coverage:
	@command -v lcov >/dev/null 2>&1 || { echo "coverage: lcov not found (brew install lcov / apt-get install lcov)" >&2; exit 1; }
	@command -v genhtml >/dev/null 2>&1 || { echo "coverage: genhtml not found" >&2; exit 1; }
	$(MAKE) clean
	$(MAKE) CFLAGS="-g -O0 -Wall --std=c99 --pedantic -fprofile-arcs -ftest-coverage" test
	@mkdir -p $(BUILD_DIR)/coverage
	# Inner $(MAKE) ... test sets PAKKA_BUILD_MODE=test, so .gcno/.gcda
	# land in build/obj-test/. Outer make sees no test goal and
	# would otherwise resolve $(OBJ_DIR) to build/obj-prod/ — capture
	# from the actual instrumented directory by name.
	lcov --capture --directory $(BUILD_DIR)/obj-test \
	     --output-file $(BUILD_DIR)/coverage/coverage.info \
	     --rc geninfo_unexecuted_blocks=1
	# --ignore-errors unused: lcov 2.x treats "exclude pattern matched
	# zero files" as a hard error. /usr/* and /Library/* are platform-
	# specific noise that may or may not be present in the .info file
	# depending on which host ran the build; suppressing the unused-
	# pattern check lets a single pattern set serve macOS + Linux.
	lcov --remove $(BUILD_DIR)/coverage/coverage.info '*/src/vendor/*' '/usr/*' '/Library/*' \
	     --ignore-errors unused \
	     --output-file $(BUILD_DIR)/coverage/filtered.info
	genhtml $(BUILD_DIR)/coverage/filtered.info --output-directory $(BUILD_DIR)/coverage/html \
	        --legend --quiet
	@echo "==> Coverage report: $(BUILD_DIR)/coverage/html/index.html"

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
	bad=`printf '%s\n' "$$all" | grep -v '^pakka_' | grep -Ev '^(_+(asan|ubsan|tsan|msan|hwasan|lsan)_|_*asan\.module_ctor|_*asan\.module_dtor)' || true`; \
	printf '%s\n' "$$all"; \
	if [ -n "$$bad" ]; then \
		echo "" >&2; \
		echo "symbol-audit FAILED: non-pakka_ defined globals in $(LIBPAKKA):" >&2; \
		printf '%s\n' "$$bad" >&2; \
		exit 1; \
	fi

$(TARGET): $(CLI_OBJECTS) $(LIBPAKKA)
	$(CC) $(CFLAGS) -o $(TARGET) $(CLI_OBJECTS) $(LIBPAKKA) $(LDLIBS)

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
	$(CC) $(CFLAGS) -o $@ tests/c_api_test.c $(LIBPAKKA) $(LDLIBS)
c_api_test: $(C_API_TEST)

# DK-codec exerciser. Unlike c_api_test (which is public-surface only),
# this test calls pakka_dk_inflate directly through src/common.h. The
# codec lives behind the library symbol audit (pakka_dk_inflate), so
# linking against $(LIBPAKKA) is enough — no separate object compile.
DK_CODEC_TEST = $(TEST_DIR)/dk_codec_test
$(DK_CODEC_TEST): tests/dk_codec_test.c $(LIBPAKKA)
	@mkdir -p $(TEST_DIR)
	$(CC) $(CFLAGS) -Iinclude -Isrc -o $@ tests/dk_codec_test.c $(LIBPAKKA) $(LDLIBS)
dk_codec_test: $(DK_CODEC_TEST)

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

test: force-relink $(TARGET) $(PAK0) $(C_API_TEST) $(DK_CODEC_TEST) symbol-audit
	CFLAGS='$(CFLAGS)' LIBPAKKA='$(LIBPAKKA)' LDLIBS='$(LDLIBS)' bats tests/

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
	./$(TARGET) -x -C $(TEST_DIR)/q3demo_raw $(Q3DEMO_ZIP) >/dev/null
	@mkdir -p $(TEST_DIR)/q3demo
	@cp $(TEST_DIR)/q3demo_raw/Quake\ 3\ Arena\ Demo/demoq3/pak0.pk3 $(Q3DEMO_PAK0_PK3)
	@echo "==> Q3 demo pak0.pk3 ready: $(Q3DEMO_PAK0_PK3)"

# GoldSrc fixture download + SHA verify + extract. Both wrappers are
# plain ZIPs; we unzip then copy the inner pak file out to the
# canonical $(GOLDSRC_*_PAK0) path. The inner paths use the names
# Valve shipped (spaces, uppercase .PAK) — we normalize to lowercase
# valve/pak0.pak on copy so the bats env vars point at a predictable
# location.
$(GOLDSRC_UPLINK_WRAPPER):
	@mkdir -p $(TEST_DIR)
	@echo "==> Downloading Half-Life Uplink demo ($(GOLDSRC_UPLINK_URL))"
	@curl -fsSL -o $(GOLDSRC_UPLINK_WRAPPER) "$(GOLDSRC_UPLINK_URL)"

verify-goldsrc-uplink: $(GOLDSRC_UPLINK_WRAPPER)
	@actual=`openssl dgst -sha256 $(GOLDSRC_UPLINK_WRAPPER) | awk '{print $$NF}'`; \
	if [ "$$actual" != "$(GOLDSRC_UPLINK_SHA256)" ]; then \
		echo "==> SHA256 mismatch on Half-Life Uplink: expected $(GOLDSRC_UPLINK_SHA256), got $$actual" >&2; \
		rm -f $(GOLDSRC_UPLINK_WRAPPER); \
		exit 1; \
	fi

$(GOLDSRC_UPLINK_PAK0): verify-goldsrc-uplink
	@command -v unzip >/dev/null 2>&1 || { echo "goldsrc fixture: unzip not found" >&2; exit 1; }
	@mkdir -p $(TEST_DIR)/hl-uplink-raw $(TEST_DIR)/hl-uplink/valve
	unzip -q -o -d $(TEST_DIR)/hl-uplink-raw $(GOLDSRC_UPLINK_WRAPPER)
	@cp "$(TEST_DIR)/hl-uplink-raw/Half-LifeUplink/valve/pak0.PAK" $(GOLDSRC_UPLINK_PAK0)
	@echo "==> Half-Life Uplink pak0.pak ready: $(GOLDSRC_UPLINK_PAK0)"

$(GOLDSRC_DAYONE_WRAPPER):
	@mkdir -p $(TEST_DIR)
	@echo "==> Downloading Half-Life: Day One demo ($(GOLDSRC_DAYONE_URL))"
	@curl -fsSL -o $(GOLDSRC_DAYONE_WRAPPER) "$(GOLDSRC_DAYONE_URL)"

verify-goldsrc-dayone: $(GOLDSRC_DAYONE_WRAPPER)
	@actual=`openssl dgst -sha256 $(GOLDSRC_DAYONE_WRAPPER) | awk '{print $$NF}'`; \
	if [ "$$actual" != "$(GOLDSRC_DAYONE_SHA256)" ]; then \
		echo "==> SHA256 mismatch on Half-Life: Day One: expected $(GOLDSRC_DAYONE_SHA256), got $$actual" >&2; \
		rm -f $(GOLDSRC_DAYONE_WRAPPER); \
		exit 1; \
	fi

$(GOLDSRC_DAYONE_PAK0): verify-goldsrc-dayone
	@command -v unzip >/dev/null 2>&1 || { echo "goldsrc fixture: unzip not found" >&2; exit 1; }
	@mkdir -p $(TEST_DIR)/hl-dayone-raw $(TEST_DIR)/hl-dayone/valve
	unzip -q -o -d $(TEST_DIR)/hl-dayone-raw $(GOLDSRC_DAYONE_WRAPPER)
	@cp "$(TEST_DIR)/hl-dayone-raw/Half-Life Day One/valve/pak0.pak" $(GOLDSRC_DAYONE_PAK0)
	@echo "==> Half-Life: Day One pak0.pak ready: $(GOLDSRC_DAYONE_PAK0)"

# Optional: full PK3 suite against id's real Q3 demo pak0.pk3. Pulls
# 93 MiB from archive.org, so kept out of `make test`. CI can opt in
# by running `make slow-test` on a single representative job.
slow-test: $(TARGET) $(Q3DEMO_PAK0_PK3) symbol-audit
	CFLAGS='$(CFLAGS)' LIBPAKKA='$(LIBPAKKA)' \
	Q3DEMO_PAK0_PK3=$(abspath $(Q3DEMO_PAK0_PK3)) bats tests/pk3_q3demo.bats

# GoldSrc parity-confirmation suite. Kept separate from slow-test
# because the GoldSrc fixtures are heavier (~138 MiB combined) and add
# a second archive.org dependency on top of the Q3 demo path. Pulls
# the Uplink (48 MiB) and Day One (90 MiB) zip wrappers, extracts
# valve/pak0.pak from each, and runs tests/pak_goldsrc.bats against
# both real Valve-built archives.
slow-test-goldsrc: $(TARGET) $(GOLDSRC_UPLINK_PAK0) $(GOLDSRC_DAYONE_PAK0) symbol-audit
	CFLAGS='$(CFLAGS)' LIBPAKKA='$(LIBPAKKA)' \
	GOLDSRC_UPLINK_PAK0=$(abspath $(GOLDSRC_UPLINK_PAK0)) \
	GOLDSRC_DAYONE_PAK0=$(abspath $(GOLDSRC_DAYONE_PAK0)) \
	bats tests/pak_goldsrc.bats
