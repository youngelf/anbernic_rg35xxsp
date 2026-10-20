# Makefile for Anbernic RG35XXSP Single-Screen Night Clock (640x480 aarch64 & x86_64 Host)

# --- Anbernic RG35XXSP (Allwinner H700 Cortex-A53 aarch64) Configuration ---
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

# --- Host Linux (x86_64 X11 640x480 Window + PulseAudio/ALSA dlopen) Configuration ---
HOST_CC      ?= gcc
HOST_CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -DHOST_X11 -D_GNU_SOURCE=1
HOST_LDFLAGS = -lX11 -lm -lpthread -ldl

# --- Targets ---
TARGET      = night_clock
HOST_TARGET = night_clock_linux
SRCS        = night_clock.c

.PHONY: all rg35xxsp linux host run-linux package clean

# Default builds the Anbernic RG35XXSP (aarch64) binary and SD card package
all: $(TARGET) package

rg35xxsp: $(TARGET)

# Build x86_64 Linux binary for local testing/debugging
linux: $(HOST_TARGET)

host: $(HOST_TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)
	$(STRIP) $(TARGET)

$(HOST_TARGET): $(SRCS)
	$(HOST_CC) $(HOST_CFLAGS) -o $(HOST_TARGET) $(SRCS) $(HOST_LDFLAGS)

run-linux: $(HOST_TARGET)
	./$(HOST_TARGET)

package: $(TARGET) night_clock.png Night_Clock.sh
	rm -rf sdcard_package
	mkdir -p sdcard_package/roms/ports/night_clock
	cp $(TARGET) night_clock.png sdcard_package/roms/ports/night_clock/
	cp Night_Clock.sh sdcard_package/roms/ports/Night_Clock.sh
	chmod +x sdcard_package/roms/ports/Night_Clock.sh sdcard_package/roms/ports/night_clock/$(TARGET)

clean:
	rm -f $(TARGET) $(HOST_TARGET)
	rm -rf sdcard_package
