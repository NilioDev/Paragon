# Linux test build of the recompiled game.  make -C host GEN=../generated
GEN ?= ../generated
CC ?= cc
CFLAGS ?= -O1 -g
GENSRC := $(wildcard $(GEN)/code_*.c) $(GEN)/tables.c
RTSRC := ../runtime/rt_core.c ../runtime/rt_libc.c ../runtime/rt_gl.c ../runtime/rt_jni.c
OBJ := $(patsubst $(GEN)/%.c,build/gen/%.o,$(GENSRC)) $(patsubst ../runtime/%.c,build/rt/%.o,$(RTSRC)) build/backend_test.o build/image.o

zen_test: $(OBJ)
	$(CC) -o $@ $(OBJ) -lm

build/gen/%.o: $(GEN)/%.c $(GEN)/funcs.h ../runtime/rt.h
	@mkdir -p build/gen
	$(CC) $(CFLAGS) -w -fwrapv -I../runtime -c $< -o $@
build/rt/%.o: ../runtime/%.c ../runtime/rt.h
	@mkdir -p build/rt
	$(CC) $(CFLAGS) -Wall -fwrapv -c $< -o $@
build/backend_test.o: backend_test.c ../runtime/rt.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wall -c $< -o $@
build/image.o: image.S $(GEN)/image.bin
	@mkdir -p build
	$(CC) -c -DGENERATED_IMAGE='"$(abspath $(GEN))/image.bin"' $< -o $@
clean:
	rm -rf build zen_test
