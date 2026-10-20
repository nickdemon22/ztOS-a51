# Checking ztMETRO's motion against a reference

ztMETRO saves every frame it draws for a while when told to on its control
pipe (`record MS`, quarter size, into `/tmp/ztrec` on the phone). These drive
that and bring the frames back, over the USB link.

    scripts/anim/cycle.sh NAME [on|off]   build, install, restart, record a launch
                                           of Settings and the return to Start
                                           (on: in 5x slow motion), fetch into
                                           build/NAME and build/NAME.png
    python3 scripts/anim/settle.py build/NAME   when each tile appears and
                                           settles in the return, in real ms
                                           (tile boxes are for the 2-column grid)

`rec.sh` runs on the phone: it unlocks with a swipe, taps the Settings tile
and the nav bar's back, all through the control pipe. Slow motion also
stretches the wait before one of our own apps is uncovered, so a slow launch
shows the tiles leaving in full.

Reference: a Lumia 820 screen capture at 60 fps (youtube RNSggiWpmxM; launch
at 0:06.2, return at 0:15.0, the return again in slow motion at 0:47.5).
What was measured from it is in the comments of `render_frame()` and
`tile_turn()` in ztmetro.c.

## What the Lumia does, measured

`lumia-track.py` follows six tiles by colour through the capture's slow-motion
return (frames extracted with ffmpeg into `g/`), giving both edges and both
edge heights per frame (`lumia-tracks.json`). From those four numbers a tile's
turn, depth and sideways offset follow directly; the eye comes out 3.4 screen
widths away, which is ztMETRO's own projection. `lumia-fit.py` fits the curves
(`lumia-fit.json`: 2.9 px rms on tiles 215 px wide) and `lumia-sim.py` prints
ztMETRO's formula beside the measurements.

Coming back to Start, each tile, for 186 ms:

- turns on the edge nearest the screen's centre line, its far side away:
  `69.8 * exp(-2.649 u) * (1-u)^0.165` degrees, u its time from 0 to 1;
- that edge `0.182 * exp(-1.656 u) * (1-u)` eye distances behind the screen;
- and offset by `(-0.476 exp(-7.157 u) + 0.193 exp(-0.849 u)) * sqrt(1-u)`
  tile widths: 0.28 to the left at first, about 0.1 past its place to the
  right a third of the way in (under its neighbour's edge), then gliding home;
- darker while turned (about three quarters edge-on).

Each row starts 50 ms after the one below, the left of two neighbours 51 ms
after the right, a small tile about 33 ms early and a fifth quicker; the top
left tile is the last. ztMETRO adds a few ms and a few percent of chance per
tile so that no two returns are the same.

Leaving (normal speed only, three frames per tile): a tile turns on its left
edge toward the viewer (10, 25, 40 degrees), grows a tenth, is swept left
(24 px of 508 at the left, 50 at the right) and is gone in about 85 ms; the
same stagger; the tapped tile alone for about 80 ms, then gone in 55.

## App list and app entrance, measured

Same capture, cut at 60 fps with `crop=648:1080:636:0` (506 px of screen =
360 points):

- **Start to the app list** (0:174.6–176.4): the two pages slide together,
  fast then settling, about 300 ms; nothing turns.
- **The list** (frame at 0:176.4): column 80 points in, squares 45 points,
  one row to the next 55, first square 17 below the status bar. A letter is
  an accent-coloured outline with the letter in its lower left; an app is its
  tile colour with its glyph, its name 13 points to the right.
- **Launching from the list** (0:130.4–132): rows leave bottom first, about
  23 ms apart, each turning on its left edge; the tapped row is alone about
  170 ms, then gone. The tiles' own rule (rows of 55 against tiles' 113 gives
  the 23 ms) — ztMETRO uses it unchanged.
- **Settings arriving** (0:76.4, frames 203–227 of `sa/`): rows from the top,
  28 ms apart, each starting 50 points right of its place and turning in as it
  slides home in about 190 ms (what is left of the way going as (1-t)^4:
  measured 1, 0.5, 0.31, 0.18, 0.055, 0.028, 0 at 0, 16, 50, 66, 100, 116,
  150 ms); the app name and title the same over 35 points and 200 ms, not
  growing. ZrnMetroCore's ENTER_* constants.

`drive.sh SCENARIO [MS] [ARGS]` wakes and unlocks the shell, goes back to the
top of Start, records MS of frames and acts one thing out through the control
pipe: `flick`, `list`, `listtap Y`, `menu Y`, `hold X Y X2 Y2` (pick a tile up
and carry it), `resize`, `launch X Y`, `shot`. `fetch.sh NAME` brings the
frames back. Frames over 25 ms while something moves are logged by ztMETRO as
`slow frame:` with what was going on.

## Pivots (tabs), measured

Office hub, 0:09.0–10.6 (`pa/`) and 0:12.0–13.6 (`pb/`), title and rows
tracked by their left edges: the finger drags the tab across, the next tab's
rows already beside its grey title; let go (past ~50 points, or flicked), the
old tab's rows are gone at once and the new tab, title and rows together,
glides home from where its title was: 272, 148, 71, 38, 17, 9 points left at
100 ms steps, e every ~150 ms. The app's name does not move; the old title
reappears at the right edge as the next one. ZrnMetroCore's PIVOT_* constants;
`drive.sh pivot` opens Settings and swipes.
