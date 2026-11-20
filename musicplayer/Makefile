# Makefile for Anbernic RG 35xxSP & Linux Desktop (x86_64) — MusicPlayer (MP3/FLAC/OGG/WAV)

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

TARGET      = musicplayer
HOST_TARGET = musicplayer_linux
SRCS        = musicplayer.c
DEPS        = musicplayer_font.h ../rg35xxsp_input.h ../third_party/dr_mp3.h ../third_party/dr_flac.h ../third_party/stb_vorbis.c

.PHONY: all rg35xxsp linux linux-x64 x64 host run-linux test package clean

all: $(TARGET) $(HOST_TARGET) package

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
	./$(HOST_TARGET) --verify ./sample_media/01_Blues_Guitar_Sessions

package: $(TARGET) musicplayer.png MusicPlayer.sh
	rm -rf sdcard_package
	mkdir -p sdcard_package/roms/ports/musicplayer
	cp $(TARGET) musicplayer.png sdcard_package/roms/ports/musicplayer/
	cp MusicPlayer.sh sdcard_package/roms/ports/MusicPlayer.sh
	chmod +x sdcard_package/roms/ports/MusicPlayer.sh sdcard_package/roms/ports/musicplayer/$(TARGET)

clean:
	rm -f $(TARGET) $(HOST_TARGET)
	rm -rf sdcard_package
