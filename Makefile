# Top-level Makefile for Anbernic RG 35xxSP Utilities & Music Apps

APPS := bard boot_logo calc chordwheel eartrainer fretboard licktrainer \
        metronome musicplayer night_clock pocketband pomodoro sightread

.PHONY: all package install clean

all:
	@for app in $(APPS); do \
		$(MAKE) -C $$app all || exit 1; \
	done
	@$(MAKE) package

# Assemble a single unified sdcard_package/roms/ports/ with all 13 apps,
# EmulationStation images/ artwork, merged gamelist.xml, and GeneralUser-GS.sf2.
package:
	@for app in $(APPS); do \
		$(MAKE) -C $$app package || exit 1; \
	done
	@rm -rf sdcard_package
	@mkdir -p sdcard_package/roms/ports
	@./install_to_sdcard.sh sdcard_package/roms/ports

# Install directly to a mounted SD card (e.g. `make install DEST=/media/SHARE/roms/ports`)
# Safely merges ports_gamelist.xml into any existing gamelist.xml on the SD card.
install:
	@if [ -z "$(DEST)" ]; then \
		echo "Usage: make install DEST=/path/to/SHARE/roms/ports" >&2; \
		exit 1; \
	fi
	@./install_to_sdcard.sh "$(DEST)"

clean:
	@for app in $(APPS); do \
		$(MAKE) -C $$app clean || exit 1; \
	done
	@rm -rf sdcard_package
