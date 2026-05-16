# Requires GNU make. On BSD: pkg_add gmake (or pkg install gmake), then run gmake.
APP_NAME="\"Pakka\""
MAJOR=1
MINOR=1
PATCH=0
BUILD_DATE="\"$(shell date +'%b %d, %Y')\""
VERSION="\"$(MAJOR).$(MINOR).$(PATCH)\""
TARGET=pakka
# _XOPEN_SOURCE=700 (POSIX.1-2008 + XSI) — needed for realpath():
# glibc gates it on __USE_XOPEN_EXTENDED, musl on _XOPEN_SOURCE, neither
# on _POSIX_C_SOURCE.
CPPFLAGS = -D_XOPEN_SOURCE=700 -D_DEBUG=1 -DAPP_NAME=$(APP_NAME) -DVERSION=$(VERSION) -DBUILD_DATE=$(BUILD_DATE)
CC=cc $(CPPFLAGS)
CFLAGS=-g -Wall --std=c99 --pedantic

SRC_DIR=src
BUILD_DIR=build
OBJ_DIR=$(BUILD_DIR)/obj
TEST_DIR=$(BUILD_DIR)/test

SOURCES=$(wildcard $(SRC_DIR)/*.c)
OBJECTS=$(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

QUAKE_URL=https://www.libsdl.org/projects/quake/data/quakesw-1.0.6.tar.gz
QUAKE_SHA256=d173e9f828b932a8160d4c65927281d0c28131cd922f0bf0d69e92a35185b499
QUAKE_TARBALL=$(TEST_DIR)/quakesw.tar.gz
PAK0=$(TEST_DIR)/pak0.pak

CLANG_TIDY ?= clang-tidy

.PHONY: all clean test test-clean distclean lint verify-tarball

all: $(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

test-clean:
	rm -rf $(TEST_DIR)/extracted $(TEST_DIR)/re_extracted $(TEST_DIR)/rebuilt.pak $(TEST_DIR)/crud $(TEST_DIR)/id1

distclean:
	rm -rf $(BUILD_DIR) $(TARGET)

lint:
	$(CLANG_TIDY) --quiet $(SOURCES) -- $(CPPFLAGS) --std=c99

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR) $(TEST_DIR):
	@mkdir -p $@

$(QUAKE_TARBALL): | $(TEST_DIR)
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

test: $(TARGET) $(PAK0)
	bats tests/
