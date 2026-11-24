# PocketBand Song Structure File Format
# =====================================
#
# Every file ending in `.song` inside this `songs/` directory is automatically
# loaded by PocketBand on startup (in alphabetical order, ignoring hidden
# dotfiles starting with `.`). Drop your own `.song` file here and press
# LEFT / RIGHT in PocketBand to jam with it!
#
# COMMENTS & WHITESPACE
# ---------------------
# - Use `#` to start a comment (full-line or inline, e.g. `# my note`).
#   Musical sharps (`F#`, `#ivdim7`, `#4`) are automatically preserved.
# - Blank lines and extra spaces around `=` are ignored.
# - Keys are case-insensitive.
#
# REQUIRED FIELDS
# ---------------
#   name     = Display title shown in the top bar and Help modal (max 44 chars)
#   chords   = Space- or `|`-separated chord progression (1 chord per bar, up to 32 bars;
#              you may repeat `chords =` across multiple lines, e.g. 4 bars per line!)
#
# OPTIONAL METADATA & GROOVE FIELDS
# ---------------------------------
#   genre       = Short badge label (e.g. BLUES 4/4, ROCK 4/4, JAZZ 3/4, BOSSA 4/4)
#   examples    = Famous songs that follow this structure (shown in the `Y` Help modal)
#   meter       = `4/4` (8 eighth-note steps per bar) or `3/4` (6 eighth-note steps per bar)
#   bpm         = Default tempo in beats per minute (45..220; default 110)
#   swing       = Eighth-note swing percentage (50 = straight 8ths, 60..66 = shuffle/swing)
#   default_key = Suggested key name: C, Db, D, Eb, E, F, F#, G, Ab, A, Bb, B
#   sections    = Optional `|`-separated labels for each 4-bar row on the Song Chart
#                 (e.g. `sections = A | A | B | TURN`, up to 8 rows / 5 chars each)
#   kit         = Instrument & comping feel profile:
#                   blues_shuffle  - Room Kit, Finger Bass, Tine E.Piano
#                   slow_blues     - Room Kit, Finger Bass, Clean Guitar + Organ pad
#                   classic_rock   - Power Kit, Picked Bass, Overdrive Guitar + Rock Organ
#                   rock_ballad    - Standard Kit, Picked Bass, Clean Guitar + Rock Organ
#                   jazz_swing     - Jazz Kit (Ride), Upright Bass, Jazz Hollowbody Guitar
#                   jazz_waltz     - Brush/Jazz Kit (Ride), Upright Bass, Tine E.Piano
#                   bossa_nova     - Brush/Jazz Kit (Ride), Upright Bass, Nylon Guitar
#                   pop_soul       - Standard Kit, Finger Bass, Tine E.Piano + Organ pad
#                   acoustic_folk  - Brush/Std Kit, Acoustic Bass, Steel Acoustic Guitar
#
# SOLOING SCALE (for the `L2` Fretboard overlay)
# ----------------------------------------------
#   scale_name = Human-readable scale name (e.g. Minor Pentatonic + Blues)
#   scale      = Space-separated scale degrees relative to the active Key root:
#                  1 (or R), b2, 2, b3, 3, 4, b5 (or #4), 5, b6, 6, b7, 7
#                Prefix any degree with `*` to highlight it as a blue/color note
#                in magenta (e.g. `scale = 1 b3 4 *b5 5 b7`).
#
# CHORD SYNTAX (`chords = ...`)
# -----------------------------
# Write chords as key-independent Roman numerals so UP / DOWN transposes your
# song seamlessly into all 12 keys:
#
#   [b|#] <ROMAN_DEGREE> [QUALITY]
#
#   Accidental : optional `b` (flat) or `#` (sharp)
#   Degree     : Uppercase (I II III IV V VI VII) defaults to Major (or Dominant 7 with `7`)
#                Lowercase (i ii iii iv v vi vii) defaults to Minor (or Minor 7 with `7`)
#   Quality    :
#     (blank)  - Major triad (uppercase) or Minor triad (lowercase)
#     m        - Minor triad
#     7        - Dominant 7th (uppercase: I7, V7, VI7) or Minor 7th (lowercase: i7, ii7, vi7)
#     maj7     - Major 7th (Imaj7, IVmaj7, bIImaj7, bVImaj7)
#     m7       - Minor 7th (im7, iim7, ivm7, biii7)
#     m7b5     - Half-diminished 7th (iim7b5, viim7b5)
#     dim7     - Diminished 7th (#ivdim7, viidim7)
#     sus4     - Suspended 4th (Vsus4, Isus4)
#
#   Example 12-bar chart:
#     chords = I7  | I7  | I7  | I7
#     chords = IV7 | IV7 | I7  | I7
#     chords = V7  | IV7 | I7  | V7
#
# 5-TRACK RHYTHM & BASS PATTERN (8 steps for 4/4, 6 steps for 3/4)
# ----------------------------------------------------------------
#   Drum tracks (`kick`, `snare`, `clhat`, `ophat`):
#     `.` or `0` = rest
#     `1` or `x` = normal hit
#     `2` or `X` = accented hit
#     (In jazz/bossa kits, `ophat` drives the Ride Cymbal / Ride Bell and
#      `clhat` drives the Hi-Hat Pedal Chick.)
#
#   Bass track (`bass`):
#     `.` or `0` = rest
#     `R`        = chord root
#     `3`        = chord 3rd (major 3rd, minor b3, or sus4 automatically)
#     `5`        = chord 5th (or b5 on m7b5 / dim7 chords)
#     `6`        = chord 6th
#     `b7` / `7` = chord 7th (b7, maj7, or bb7 automatically)
#     `8`        = octave root
#
# MINIMAL CUSTOM SONG EXAMPLE (save as e.g. `14_my_groove.song`)
# --------------------------------------------------------------
#   name = My 12-Bar Jam
#   genre = ROCK 4/4
#   examples = Original Jam
#   meter = 4/4
#   bpm = 116
#   swing = 50
#   kit = classic_rock
#   scale_name = Minor Pentatonic (1 b3 4 5 b7)
#   scale = 1 b3 4 5 b7
#   sections = A | A | B
#   chords = i  | bVII | bVI | bVII
#   chords = i  | bVII | bVI | V7
#   chords = IV | bVII | i   | V7
#   kick  = 2 . . 1 2 . . .
#   snare = . . 2 . . . 2 .
#   clhat = 2 1 2 1 2 1 2 .
#   ophat = . . . . . . . 1
#   bass  = R R R 5 R R 8 5
