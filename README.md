# Anbernic RG 35xxSP
Hand-written utilities for the Anbernic RG 35xxSP

The RG 35xxSP is just a simple Linux device. It can be used to play
retro games, but since it is a Linux machine, you can write simple
utilities that can be displayed on the screen, and controlled using
DPAD and A/B/X/Y keys.  I'm running Knulli, which is based on
Batocera, but this codebase is a good starting point even if you are
using the standard firmware, as it is a simple Linux ARM binary.

Here are a few utilities that can be built both for the machine, and
also compile natively on Linux x11 so you can test them out before
copying it on the SD-card for running on your RG 35xxSP.


## [Metronome](metronome/)

A simple metronome that shows some common time signatures, and plays a
tick on the beat.

![Screenshot of metronome](/metronome/promo/metronome_grab.png)


## [Pomodoro](pomodoro/)

A work timer to let folks know that you are working.

![Screenshot of pomodoro](/pomodoro/promo/pomodoro_grab.png)
