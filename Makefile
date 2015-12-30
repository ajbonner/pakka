APP_NAME = "\"Pakka\""
MAJOR = 1
MINOR = 0
BUILD_DATE = "\"$(shell date +'%b %d, %Y')\""
VERSION = "\"$(MAJOR).$(MINOR)\""
TARGET=pakka
CPPFLAGS = -DAPP_NAME=$(APP_NAME) -DVERSION=$(VERSION) -DBUILD_DATE=$(BUILD_DATE)
CC=cc $(CPPFLAGS)
CFLAGS=-g -Wall 
SRC_DIR=src

all: clean ${TARGET}

clean: 
	rm -rf src/*.o ${TARGET}

${TARGET}:
	${CC} -o ${TARGET} ${SRC_DIR}/**.c

