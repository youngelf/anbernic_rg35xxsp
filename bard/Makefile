# Makefile for Anbernic RG 35xxSP & Linux Desktop (x86_64) — Bard EPUB & Text Reader

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

MINIZ_SRC    = ../third_party/miniz.c
MINIZ_HDR    = ../third_party/miniz.h

TARGET       = bard
HOST_TARGET  = bard_linux
SRCS         = bard.c

.PHONY: all rg35xxsp linux linux-x64 x64 host run-linux test package clean

all: $(TARGET) $(HOST_TARGET) package

rg35xxsp: $(TARGET)

linux: $(HOST_TARGET)

linux-x64: $(HOST_TARGET)

x64: $(HOST_TARGET)

host: $(HOST_TARGET)

miniz_arm.o: $(MINIZ_SRC) $(MINIZ_HDR)
	$(CC) -O2 -std=gnu99 -mcpu=cortex-a53 -D_GNU_SOURCE=1 $(SYSROOT_FLAGS) -Wno-unused-function -Wno-unused-parameter -c $(MINIZ_SRC) -o miniz_arm.o

miniz_host.o: $(MINIZ_SRC) $(MINIZ_HDR)
	$(HOST_CC) -O2 -std=gnu99 -D_GNU_SOURCE=1 -Wno-unused-function -Wno-unused-parameter -c $(MINIZ_SRC) -o miniz_host.o

$(TARGET): $(SRCS) bard_font.h ../rg35xxsp_input.h miniz_arm.o
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) miniz_arm.o $(LDFLAGS)
	$(STRIP) $(TARGET)

$(HOST_TARGET): $(SRCS) bard_font.h ../rg35xxsp_input.h miniz_host.o
	$(HOST_CC) $(HOST_CFLAGS) -o $(HOST_TARGET) $(SRCS) miniz_host.o $(HOST_LDFLAGS)

run-linux: $(HOST_TARGET)
	./$(HOST_TARGET)

test: $(HOST_TARGET)
	./$(HOST_TARGET) --verify ./media

package: $(TARGET) bard.png Bard.sh
	rm -rf sdcard_package
	mkdir -p sdcard_package/roms/ports/bard/media
	cp $(TARGET) bard.png sdcard_package/roms/ports/bard/
	cp media/*.epub sdcard_package/roms/ports/bard/media/
	cp Bard.sh sdcard_package/roms/ports/Bard.sh
	chmod +x sdcard_package/roms/ports/Bard.sh sdcard_package/roms/ports/bard/$(TARGET)

clean:
	rm -f $(TARGET) $(HOST_TARGET) miniz_arm.o miniz_host.o
	rm -rf sdcard_package
