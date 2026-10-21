# Makefile for Anbernic RG 35xxSP & Linux Desktop (x86_64) — PocketBand
# Builds:
#   1. pocketband       : 64-bit ARM (aarch64) binary for Anbernic RG 35xxSP
#   2. pocketband_linux : Native Linux x86_64 desktop X11 + PulseAudio/ALSA binary

TOOLCHAIN_ROOT := /usr/local/google/home/viki/.cache/aarch64_toolchain
ARM_BIN        := $(TOOLCHAIN_ROOT)/usr/bin
ARM_ENV        := PATH="$(ARM_BIN):$$PATH" LD_LIBRARY_PATH="$(TOOLCHAIN_ROOT)/usr/lib/x86_64-linux-gnu:$(TOOLCHAIN_ROOT)/usr/lib"
ARM_CC         := $(ARM_ENV) aarch64-linux-gnu-gcc-14
ARM_STRIP      := $(ARM_ENV) aarch64-linux-gnu-strip
ARM_SYSROOT    := --sysroot="$(TOOLCHAIN_ROOT)" \
                  -isystem "$(TOOLCHAIN_ROOT)/usr/aarch64-linux-gnu/include" \
                  -B"$(TOOLCHAIN_ROOT)/usr/aarch64-linux-gnu/lib" \
                  -B"$(TOOLCHAIN_ROOT)/usr/lib/gcc-cross/aarch64-linux-gnu/14" \
                  -L"$(TOOLCHAIN_ROOT)/usr/aarch64-linux-gnu/lib"

HOST_CC        := gcc
CFLAGS_COMMON  := -O2 -Wall -Wextra -std=gnu99

TARGET_ARM     := pocketband
TARGET_LINUX   := pocketband_linux
PACKAGE_DIR    := sdcard_package
SRC            := pocketband.c

.PHONY: all arm rg35xxsp linux linux-x64 x64 host package run-linux clean

all: $(TARGET_ARM) $(TARGET_LINUX) package

arm: $(TARGET_ARM)

rg35xxsp: $(TARGET_ARM)

linux: $(TARGET_LINUX)

linux-x64: $(TARGET_LINUX)

x64: $(TARGET_LINUX)

host: $(TARGET_LINUX)

$(TARGET_ARM): $(SRC) pocketband_font.h
	$(ARM_CC) $(CFLAGS_COMMON) -mcpu=cortex-a53 -DTARGET_RG35XXSP -D_GNU_SOURCE=1 $(ARM_SYSROOT) -o $@ $(SRC) -lm -lpthread -ldl
	$(ARM_STRIP) $@

$(TARGET_LINUX): $(SRC) pocketband_font.h
	$(HOST_CC) $(CFLAGS_COMMON) -DHOST_X11 -D_GNU_SOURCE=1 -o $@ $(SRC) -lX11 -lm -lpthread -ldl

package: $(TARGET_ARM) PocketBand.sh pocketband.png
	rm -rf $(PACKAGE_DIR)
	mkdir -p $(PACKAGE_DIR)/roms/ports/pocketband
	cp $(TARGET_ARM) pocketband.png $(PACKAGE_DIR)/roms/ports/pocketband/
	cp PocketBand.sh $(PACKAGE_DIR)/roms/ports/PocketBand.sh
	chmod +x $(PACKAGE_DIR)/roms/ports/PocketBand.sh $(PACKAGE_DIR)/roms/ports/pocketband/$(TARGET_ARM)

run-linux: $(TARGET_LINUX)
	./$(TARGET_LINUX)

clean:
	rm -rf $(TARGET_ARM) $(TARGET_LINUX) $(PACKAGE_DIR)
