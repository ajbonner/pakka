TARGET=pakker
CC=cc
CFLAGS=-g -Wall -c99
SRC_DIR=src

all: clean ${TARGET}

clean: 
	rm -rf src/*.o ${TARGET}

${TARGET}:
	${CC} -o ${TARGET} ${SRC_DIR}/**.c

