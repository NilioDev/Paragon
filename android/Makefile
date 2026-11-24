# Build libmerkava.so for Android.  make -C android ABI=arm64-v8a GEN=../generated
NDK ?= /opt/android-ndk-r27c
GEN ?= ../generated
ABI ?= arm64-v8a
API ?= 24
TC := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
TRIPLE_arm64-v8a := aarch64-linux-android
TRIPLE_x86_64 := x86_64-linux-android
CC := $(TC)/clang --target=$(TRIPLE_$(ABI))$(API)
CFLAGS := -O2 -fPIC -fwrapv -ffunction-sections -fdata-sections
OUT := build/$(ABI)
GENSRC := $(wildcard $(GEN)/code_*.c) $(GEN)/tables.c
RTSRC := ../runtime/rt_core.c ../runtime/rt_libc.c ../runtime/rt_gl.c ../runtime/rt_jni.c ../runtime/overrides.c ../runtime/present.c ../runtime/widescreen.c
OBJ := $(patsubst $(GEN)/%.c,$(OUT)/gen/%.o,$(GENSRC)) $(patsubst ../runtime/%.c,$(OUT)/rt/%.o,$(RTSRC)) $(OUT)/backend_android.o $(OUT)/image.o

$(OUT)/libmerkava.so: $(OBJ)
	$(CC) -shared -o $@ $(OBJ) -Wl,--no-undefined -Wl,--gc-sections -Wl,-z,max-page-size=16384 -lGLESv1_CM -llog -lm
	$(TC)/llvm-strip --strip-unneeded $@

$(OUT)/gen/%.o: $(GEN)/%.c $(GEN)/funcs.h ../runtime/rt.h
	@mkdir -p $(OUT)/gen
	$(CC) $(CFLAGS) -w -I../runtime -c $< -o $@
$(OUT)/rt/%.o: ../runtime/%.c ../runtime/rt.h
	@mkdir -p $(OUT)/rt
	$(CC) $(CFLAGS) -Wall -I../runtime -I$(GEN) -c $< -o $@
$(OUT)/backend_android.o: backend_android.c ../runtime/rt.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Wall -c $< -o $@
$(OUT)/image.o: ../host/image.S $(GEN)/image.bin
	@mkdir -p $(OUT)
	$(CC) -c -DGENERATED_IMAGE='"$(abspath $(GEN))/image.bin"' $< -o $@
clean:
	rm -rf build
