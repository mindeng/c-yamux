INC_DIR=inc
SRC_DIR=src
OBJ_DIR=obj
BIN_DIR=bin

ifeq ($(CC),)
	CC=clang
endif
AR=ar

ifeq ($(CC),clang)
	WALL=everything
else
	WALL=all
endif

OUTNAME=yamux.a
OUTPATH=$(BIN_DIR)/$(OUTNAME)
TSTNAME=ytest
TSTPATH=$(BIN_DIR)/$(TSTNAME)

CCFLAGS=-I$(INC_DIR) -W$(WALL) -Wno-vla
LIBS=

default: release

all: makeobjdirs
all: $(OUTPATH) $(TSTPATH)

$(TSTPATH): $(OUTPATH) $(OBJ_DIR)/main.o
	$(CC) -o $@ \
        $(OBJ_DIR)/frame.o $(OBJ_DIR)/session.o $(OBJ_DIR)/stream.o \
        $(OBJ_DIR)/main.o \
        $(CCFLAGS) $(LIBS)

$(OUTPATH): \
    $(OBJ_DIR)/frame.o $(OBJ_DIR)/session.o $(OBJ_DIR)/stream.o
	$(AR) rcs $@ \
        $(OBJ_DIR)/frame.o $(OBJ_DIR)/session.o $(OBJ_DIR)/stream.o

debug: CCFLAGS += -DDEBUG -g
debug: all

release: CCFLAGS += -DRELEASE -O3
release: cleanbins all

makeobjdirs:
	@if ! [ -d "$(BIN_DIR)" ]; then	mkdir -p "$(BIN_DIR)"; fi
	@if ! [ -d "$(OBJ_DIR)" ]; then	mkdir -p "$(OBJ_DIR)"; fi

cleanbins:
	@if [ -f "$(OUTPATH)" ]; then	rm "$(OUTPATH)"; fi

$(OBJ_DIR)/frame.o: $(SRC_DIR)/frame.c
	$(CC) -c $< -o $@ $(CCFLAGS) $(LIBS)
$(OBJ_DIR)/session.o: $(SRC_DIR)/session.c
	$(CC) -c $< -o $@ $(CCFLAGS) $(LIBS)
$(OBJ_DIR)/stream.o: $(SRC_DIR)/stream.c
	$(CC) -c $< -o $@ $(CCFLAGS) $(LIBS)

$(OBJ_DIR)/main.o: $(SRC_DIR)/main.c
	$(CC) -c $< -o $@ $(CCFLAGS) $(LIBS)

clean: cleanbins
	-find "$(OBJ_DIR)" -type f -name "*.o" | xargs rm -v

.PHONY: clean all debug release

