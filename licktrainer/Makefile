# Makefile for Anbernic RG35XXSP Guitar Speed-Builder & Lick Coach (640x480 aarch64 & x86_64 Host)

AARCH64_ROOT ?= $(HOME)/.cache/aarch64_toolchain
CACHED_GCC   := $(wildcard $(AARCH64_ROOT)/usr/bin/aarch64-linux-gnu-gcc-14)

ifneq ($(CACHED_GCC),)
    AARCH64_PATH   = $(AARCH64_ROOT)/usr/bin:$(PATH)
    AARCH64_LD_LIB = $(AARCH64_ROOT)/usr/lib/x86_64-linux-gnu:$(AARCH64_ROOT)/usr/lib
    CC    = PATH="$(AARCH64_PATH)" LD_LIBRARY_PATH="$(AARCH64_LD_LIB)" aarch64-linux-gnu-gcc-14
    STRIP = PATH="$(AARCH64_PATH)" LD_LIBRARY_PATH="$(AARCH64_LD_LIB)" aarch64-linux-gnu-strip
    SYSROOT_FLAGS = --sysroot="$(AARCH64_ROOT)" \
        -isystem "$(AARCH64_ROOT)/usr/aarch64-linux-gnu/include" \
        -B"$(AARCH64_ROOT)/usr/aarch64-linux-gnu/lib" \
        -B"$(AARCH64_ROOT)/usr/lib/gcc-cross/aarch64-linux-gnu/14" \
        -L"$(AARCH64_ROOT)/usr/aarch64-linux-gnu/lib"
else
    CC    = aarch64-linux-gnu-gcc
    STRIP = aarch64-linux-gnu-strip
    SYSROOT_FLAGS =
endif

CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -mcpu=cortex-a53 -DTARGET_RG35XXSP -D_GNU_SOURCE=1 $(SYSROOT_FLAGS)
LDFLAGS = -lm -lpthread -ldl

HOST_CC      ?= gcc
HOST_CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -DHOST_X11 -D_GNU_SOURCE=1
HOST_LDFLAGS = -lX11 -lm -lpthread -ldl

TARGET      = licktrainer
HOST_TARGET = licktrainer_linux
SRCS        = licktrainer.c
DEPS        = licktrainer_font.h ../rg35xxsp_input.h ../rg35xxsp_sf2.h ../third_party/tsf.h

.PHONY: all arm rg35xxsp linux linux-x64 x64 host run-linux test screenshots package clean

all: $(TARGET) $(HOST_TARGET) package

arm: $(TARGET)

rg35xxsp: $(TARGET)

linux: $(HOST_TARGET)

linux-x64: $(HOST_TARGET)

x64: $(HOST_TARGET)

host: $(HOST_TARGET)

$(TARGET): $(SRCS) $(DEPS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)
	$(STRIP) $(TARGET)

$(HOST_TARGET): $(SRCS) $(DEPS)
	$(HOST_CC) $(HOST_CFLAGS) -o $(HOST_TARGET) $(SRCS) $(HOST_LDFLAGS)

run-linux: $(HOST_TARGET)
	./$(HOST_TARGET)

test: $(HOST_TARGET)
	./$(HOST_TARGET) --verify

screenshots: $(HOST_TARGET)
	mkdir -p promo
	./$(HOST_TARGET) --screenshot promo

package: $(TARGET) licktrainer.png LickTrainer.sh
	rm -rf sdcard_package
	mkdir -p sdcard_package/roms/ports/licktrainer
	cp $(TARGET) licktrainer.png sdcard_package/roms/ports/licktrainer/
	cp LickTrainer.sh sdcard_package/roms/ports/LickTrainer.sh
	chmod +x sdcard_package/roms/ports/LickTrainer.sh sdcard_package/roms/ports/licktrainer/$(TARGET)

clean:
	rm -f $(TARGET) $(HOST_TARGET)
	rm -rf sdcard_package
