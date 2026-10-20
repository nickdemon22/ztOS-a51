# ztOS SDK

Write apps for ztOS in Python with Qt (PySide6), try them on the laptop, and
install them on the phone as a single runnable binary that shows up in the app
list like any other app.

```
ztos-sdk new "Notes"        # a new app in ./notes, from the template
cd notes
ztos-sdk run                # try it here, in a 360x640 window (Escape = back)
ztos-sdk designer           # draw pages in Qt Designer (form.ui)
ztos-sdk deploy             # build, install on the phone, open it there
ztos-sdk logs -f            # what it prints, on the phone
```

## Python API and native rendering

Apps keep using the Python `ztos` API. Qt handles layout, input, accessibility,
and prepares cached widget/text images. The C backend in
[`native/renderer.c`](native/renderer.c) uploads those images and draws their
movement through OpenGL: texture management, vertex generation, perspective,
and premultiplied blending run in C. Python sends a batch of quads per frame.
Content changes still require a Qt raster pass; this is not a full replacement
of Qt's widget renderer. On the Galaxy S9, the system environment sets
`PAN_MESA_DEBUG=noafbc`: compressed texture uploads produced intermittent
Panfrost job faults and missing page headers during repeated launches. GLES
rendering and GPU animation remain enabled with uncompressed textures. Entrance curves and shader compilation are shared with
the native Settings renderer through `ztmetro/core/zrnmetrorender.h`.

`ztos-sdk run` builds the host C library automatically. It needs `make`, a C
compiler, `pkg-config`, and GLES2 development files. `install` and `deploy`
build the phone library over SSH when the source changes, using the same tools
on the phone. Later offline `build` commands reuse that cached target library.
You can also cross-compile it with `make -C sdk/native ARCH=aarch64 CC=...`
and the appropriate GLES2 sysroot/pkg-config configuration. The build checks a
source hash, so changes to the shared header also trigger a rebuild.

Each app archive includes its target library. The runtime extracts it into a
cache named by its SHA-256 hash before loading it with `ctypes`; the warm
runtime receives the same library during installation. No GL context is made
until the app opens. Offscreen runs and devices without a working GPU retain
the existing software path.

## What a built app is

`ztos-sdk build` writes `dist/<id>`: one ELF file for aarch64, usually under
100 KB. It is a small launcher ([launcher/launcher.c](launcher/launcher.c))
with a zip archive appended to it. The archive holds the app's code, its
pictures, every `.ui` page compiled to Python, and the `ztos` runtime. Python
reads zip archives from their end, so the launcher just runs the phone's
`python3` on its own file. Before it does, it sets Qt to Wayland and turns off
Qt's own title bar.

Qt itself is not inside the binary. The phone provides it (`pyside6`,
`qt6-wayland`, `qt6-svg` from Arch Linux ARM), which is why apps stay small. The native renderer can be built on the phone
during deployment, so a phone cross compiler is optional.

`ztos-sdk install` copies files into the alarm user's home on the phone:

| what | where |
|---|---|
| the binary | `~/.local/share/ztos/apps/<id>` |
| the app list entry | `~/.local/share/applications/<id>.desktop` |
| the icon | `~/.local/share/icons/hicolor/scalable/apps/<id>.svg` |
| what it prints | `~/.local/state/ztos/<id>.log` (the run before that in `.log.1`) |

Then it tells the shell to reread the app list. Nothing needs root, and
`ztos-sdk uninstall` removes all of it.

## Setting up

On the laptop you need Python 3.11 or newer and PySide6 (`pacman -S pyside6`).
Qt Designer is optional (`qt6-tools`). Put `sdk/bin` on your PATH, or link
`sdk/bin/ztos-sdk` into `~/.local/bin`.

The phone needs Qt for Python. Check it, then install what is missing (this
needs internet on the phone, and root):

```
ztos-sdk doctor
ztos-sdk doctor --fix       # pacman -S --needed python pyside6 qt6-wayland qt6-svg
```

The phone is `alarm@172.16.42.1` (USB networking). Point at another address
with `--device user@host`, `$ZTOS_DEVICE`, or `[device] host` in `ztos.toml`.
ssh asks for the password. Set `$ZTOS_PASSWORD` to skip the prompt, and
`$ZTOS_ROOT_PASSWORD` for `doctor --fix`. Neither is ever written to a file.

## ztos.toml

```toml
[app]
id = "com.example.notes"   # reverse-domain; the shell knows the window by it
name = "Notes"             # app list and tiles
caps = "NOTES"             # the small line over each page title
version = "0.1.0"
entry = "main.py"
icon = "icon.svg"          # white on transparent; svg, or a 256 px png

[build]
include = ["**/*.py", "**/*.ui", "**/*.svg", "**/*.png", "**/*.json"]
exclude = []
dependencies = []          # pure-Python packages from PyPI, bundled in
```

Ids starting with `zt` or `zrn` are reserved for the system's own apps.
Packages with compiled code cannot be bundled. Install those on the phone with
pacman (`python-...`) and import them as normal.

## The `ztos` module

It gives apps the same look and motion as the phone's own apps (ZrnMetroCore):
a black page, the app name in small capitals, a large title, the accent colour
from Settings, lists that coast under a finger, rows that tilt away from a
press, and pages that swing in like a turnstile. Pages are 360 points wide;
the shell scales them to the screen.

```python
import ztos

app = ztos.App()

class Home(ztos.Page):
    def __init__(self):
        super().__init__("notes")
        self.entry("new note", "start writing", lambda: app.push(Editor()))
        self.toggle("Sync", on=True, on_change=self.sync)
        self.slider("Text size", 60, on_change=print)

    def shown(self):          # the page has arrived: load what it shows
        ...

    def sync(self, on):
        ztos.notify("Notes", "sync on" if on else "sync off")

app.run(Home())
```

| piece | what it is |
|---|---|
| `App()` | the window. `run(page)`, `push(page)` (turnstile in), `pop()`, `replace(page)`, `back()` |
| `Page(title)` | a title and a scrolling list. `entry`, `toggle`, `slider`, `textbox`, `button`, `text`, `section`, `space`, `round_buttons`, `bar` (app bar), `add(any QWidget)`, `clear()`. Override `shown()` and `back()` |
| `Pivot([pages])` | pages side by side under one row of titles; swipe or tap a title |
| `Entry(title, detail, on_tap, on_hold)` | a list row; `set_title`, `set_detail` |
| `Toggle(title, on, on_change)` | the on/off switch row |
| `Slider(caption, value, on_change)` | caption, value, and a bar to drag |
| `TextBox(caption, text, on_change, on_submit, password)` | brings up the keyboard when tapped |
| `RoundButton(glyph, label, on_tap)` | glyphs: add, save, edit, delete, send, cancel, check, back, refresh, search, call, message, more, share, settings, play, pause; any short text; or an `.svg` of the app's |
| `menu(widget, [(text, callback)])` | the hold menu under a row |
| `haptic(kind)` | tick, tap, click, heavy; at the strengths set in Settings > haptics |
| `notify(title, text)` | a notification in the action centre |
| `accent()` | the accent colour, as a QColor |
| `resource(path)` | one of the app's files, as bytes (works inside the binary) |
| `load_ui("form.ui")` | a Qt Designer page as a widget; its parts are on `.ui` |
| `on_device()` | True inside the ztMETRO shell |

| `ProgressBar(value, caption=...)` | a thin accent bar; `set_value()` glides to the new value, `None` shows the loading dots |
| `LoadingDots()`, `page.busy(True)` | the phone's five loading dots, alone or across the top of a page |
| `message(title, text, buttons, on_result)` | a message from the top: the page dims, the panel swings down; `on_result` gets the button's index, or `None` for back |
| `Dialog(title, text, buttons, on_result, content=widget)` | the same, with your own widget inside |

### ztOS

| call | what it does |
|---|---|
| `performance("eco" / "normal" / "big")` | tells zrnperfd how to treat the app while it is in front. **eco**: its load never brings the big cores in. **big**: they stay open to it (games, camera). Set it once with `performance = "eco"` in `ztos.toml` |
| `battery()` | `{"percent": 0–100, "charging": bool, "full": bool}` |
| `store(name="settings")` | a dict saved on the phone for this app (`~/.local/share/ztos/data/<id>/`): change it, then `.save()` |
| `launch(command)` | opens another app in the shell |
| `haptic`, `notify`, `osk`, `accent` | as above |

Ordinary Qt widgets work too. The theme styles push buttons, line edits, check
boxes, combo boxes, lists, progress bars and sliders to match, so a Designer
page fits in without extra work.

## How things move

The runtime ports the phone's own toolkit (`ztmetro/core/zrnmetrocore.c`):
same numbers, same rules.

- **One finger, one owner.** A touch that stays put belongs to the widget (a
  tap, or a hold after 500 ms). Once it moves 8 points it is a drag: whatever
  it pressed lets go at once, and the drag belongs to the list or to the
  pivot. On a pivot, only a nearly flat stroke (within 20 degrees, over 18
  points) goes across; anything else scrolls.
- **Lists** follow the finger, give at 40 % past either end, coast when let
  go, and spring back from an overscroll.
- **Pivots** move whole under the finger. The titles follow at a title's
  width per screen and light up past half way. When let go, they settle on a
  critically damped spring, and a flick carries on to the next tab.
- **Page changes** are the row-by-row turnstile: rows leave from the top with
  the tapped row last, then a moment of black, then the new rows turn in.
  Going back, both run bottom-up. The first page feathers in the way Settings
  does, at the moment the shell says, so an app's window id is `ztos.<id>`.
  That prefix tells the shell the app brings its own rows in.
- **Pressed rows and buttons** lean toward the finger and sink a little.

Everything that moves is drawn by the GPU. A child OpenGL surface over the
app shows pictures of the pages, made while the app is idle, and only says
where each one goes. Per frame that costs about 0.5 ms of CPU, against 7–10 ms
for Qt redrawing and resending the whole 1440×2560 window. If a picture is not
ready when a finger arrives, that one gesture is drawn by the CPU rather than
waiting. Where there is no OpenGL (`ZTOS_GL=0`, or the offscreen platform),
everything is drawn by the CPU at half resolution.

## Starting fast: the warm runtime

Loading Qt for Python costs about a second of CPU on the phone's efficiency
cores, at every start. So the first SDK app started after boot (or after an
install) starts `ztos-zygote` as well. That one process loads Qt, the fonts
and the SDK runtime once, then sleeps on a socket at no cost. Every app
launcher after that asks it to fork the app, and stays behind as the app's
stand-in: signals go through, and the exit code comes back.

An app uses the preloaded runtime only if its bundled copy is the same file;
otherwise it loads its own. The warm runtime holds about 50–60 MB.
`ZTOS_ZYGOTE=0` turns it off for one launch.

The shell knows SDK windows by their id (`ztos.<id>`), not by process ID, so
tapping the tile of a running SDK app brings it back.

## Limits

- **Python is compiled at every start.** The zip holds `.py` files, and Python
  cannot cache bytecode inside an archive. That costs roughly 0.1–0.3 s per
  launch on the efficiency cores.
- **Qt Quick/QML is not themed.** The look covers Qt Widgets. QML apps run, but
  they get no ztOS styling.
- **While a list moves, it shows its picture.** Widgets that change during a
  scroll (a clock ticking, a download bar) show the change when the list
  stops.
