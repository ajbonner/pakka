APP_NAME="\"Pakka\""
MAJOR=1
MINOR=0
BUILD_DATE="\"$(shell date +'%b %d %Y')\""
VERSION="\"$(MAJOR).$(MINOR)\""
TARGET=pakka
CPPFLAGS = -D_DEBUG=1 -DAPP_NAME=$(APP_NAME) -DVERSION=$(VERSION) -DBUILD_DATE=$(BUILD_DATE)
CC=cc $(CPPFLAGS)
CFLAGS=-g -Wall --std=c99 --pedantic -D_DEFAULT_SOURCE
SRC_DIR=src
SOURCES=$(SRC_DIR)/common.c $(SRC_DIR)/debug.c $(SRC_DIR)/main.c $(SRC_DIR)/filesystem.c $(SRC_DIR)/options.c \
	$(SRC_DIR)/pakfile.c $(SRC_DIR)/string.c
OBJECTS=$(SOURCES:.c=.o)

all: $(TARGET)

clean:
	rm -rf $(OBJECTS) $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $^

$(OBJECTS): src/%.o : src/%.c
	$(CC) $(CFLAGS) -c $< -o $@
