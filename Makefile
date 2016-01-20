APP_NAME="\"Pakka\""
MAJOR=1
MINOR=0
BUILD_DATE="\"$(shell date +'%b %d, %Y')\""
VERSION="\"$(MAJOR).$(MINOR)\""
TARGET=pakka
CPPFLAGS = -DAPP_NAME=$(APP_NAME) -DVERSION=$(VERSION) -DBUILD_DATE=$(BUILD_DATE)
CC=cc $(CPPFLAGS)
CFLAGS=-g -Wall --std=c99 --pedantic
SRC_DIR=src
SOURCES=$(wildcard $(SRC_DIR)/*.c)
OBJECTS=$(SOURCES:.c=.o)

all: $(TARGET)

clean:
	rm -rf $(OBJECTS) $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $^

$(OBJECTS): src/%.o : src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

