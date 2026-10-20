# ztOS / ztMETRO for Samsung Galaxy A51 (SM-A515F)

Port of the Windows Phone 8.1-styled **ztOS** environment and **ztMETRO** Wayland/wlroots compositor to the **Samsung Galaxy A51** (`samsung-a51`) running postmarketOS / Nura (Alpine Linux edge, downstream Linux kernel 4.14.113).

This repository contains the full source code, hardware adaptation patches, device porting recipes (`pmaports`), and automated deployment scripts to run an authentic, fluid Metro UI natively on the Galaxy A51's 1080x2400 Super AMOLED display.

---

## Hardware Specifications

| Attribute | Specification |
| :--- | :--- |
| **Manufacturer** | Samsung |
| **Model** | Galaxy A51 (`SM-A515F`, `SM-A515W`) |
| **Codename** | `samsung-a51` |
| **SoC** | Samsung Exynos 9611 Octa-core (4x Cortex-A73 @ 2.3 GHz + 4x Cortex-A53 @ 1.7 GHz) |
| **GPU** | ARM Mali-G72 MP3 (Bifrost v2/v3 architecture, Job Manager [JM]) |
| **Kernel Driver** | ARM `mali_kbase` UAPI 11.35 (`/dev/mali0`) |
| **Display** | 6.5" 1080x2400 Super AMOLED (20:9 ratio, Infinity-O center punch-hole) |
| **Architecture** | `aarch64` |
| **Base System** | postmarketOS / Nura (Alpine Linux Edge, musl libc) |
| **Kernel** | Downstream Linux `4.14.113-samsung-a51` |

---

## Hardware & Feature Support Status

Comparison between the base upstream Nura Wiki status and our optimized ztOS implementation:

| Component / Feature | Base Wiki Status | ztOS Port Status | Implementation Details |
| :--- | :--- | :--- | :--- |
| **Display & Super AMOLED** | Works | **Works (Full 1080x2400)** | Native 1080x2400 panel resolution with calibrated status bar offsets clearing the front punch-hole camera. |
| **Touchscreen** | Works | **Works** | Multi-touch capacitive digitizer supported natively by kernel input drivers. |
| **Framebuffer & Refresh** | *Required `msm-fb-refresher`* | **Works (Native DECON)** | **Fixed:** Removed `msm-fb-refresher` (Qualcomm loop daemon), which consumed 90% CPU on nice -20. The Samsung Exynos DECON controller refreshes autonomously without user-space polling. |
| **2D/3D Rendering** | Untested | **Works (Accelerated llvmpipe)** | Multithreaded Mesa llvmpipe (`LP_NUM_THREADS=4`) rasterizing across all 4 big Cortex-A73 cores, bridged via a custom ION allocator. |
| **Window Compositing** | N/A | **Works (ztMETRO)** | Custom Wayland compositor based on wlroots 0.20, patched at opcode level for native 1080x2400 presentation. |
| **Screen Sleep / Wake** | Untested | **Works (AMOLED True Black)** | `KEY_POWER` activates screen sleep by clearing to pure black `#000000` (turning Super AMOLED pixels off), waking back up to the lock screen with zero X11 cursor glitching. |
| **Battery & Charging** | Works | **Works** | Full battery percentage, voltage, and current monitoring via sysfs/UPower. |
| **USB Networking (SSH)** | Works | **Works** | RNDIS / CDC-Ethernet network interface (`172.16.42.1`) enabled by default for terminal and deployment access. |
| **Wi-Fi** | Works | **Works** | Broadcom/Samsung proprietary firmware blobs packaged in `firmware-samsung-a51`. |
| **Audio** | Untested | **Partial** | ALSA output channels available (`sec_audio`); system sounds routed via `ztos-sounds`. |
| **File Management** | Untested | **Works (Files / Thunar)** | Integrated GTK3 Wayland-native file manager with bwrap sandbox bypass for Linux 4.14 compatibility. |
| **Screenshot Capture** | N/A | **Works (`screenshot`)** | Wayland `wlr-screencopy-v1` integration with `grim` CLI tool and timed capture support. |
| **Modem (Calls / SMS / Data)** | Untested | **Untested** | Modem daemons (`modemmanager`) functional; audio routing and SIM switching require testing. |
| **Bluetooth / GPS / Camera** | Untested | **Untested** | Kernel drivers present; user-space stacks not yet mapped to ztOS applications. |

---

## Technical Architecture & Key Fixes

### 1. Eliminating the CPU Bottleneck (`msm-fb-refresher`)
In early postmarketOS builds, the `msm-fb-refresher` daemon (intended for Qualcomm MSM hardware) ran in an infinite unthrottled loop issuing `MSM_FB_REFRESH` ioctls at real-time priority (`nice -20`). On Exynos 9611, these unsupported ioctls saturated the bus, keeping an entire CPU core at 100% and pushing system `load average` to ~17.0.
- **Fix:** Neutralized `msm-fb-refresher`. The native Samsung DECON display controller updates frames directly from `/dev/fb0`, dropping idle CPU usage from 90% to ~0.5%.

### 2. Custom ION DMA-BUF Memory Allocator
Under Wayland over X11 (`WLR_BACKENDS=x11`), GLES2 rendering via llvmpipe requires shared buffer presentation. Standard DRM render nodes failed with `Assertion failed: buffer->impl->get_dmabuf` and X11 shared memory crashed due to `st_size == 0`.
- **Fix:** Built a custom ION allocator bridging Samsung's `/dev/ion` DMA-BUF allocator to `memfd` shared memory handles (`xcb_shm_attach_fd`), providing zero-copy buffer passing between llvmpipe and the X11 surface.
- **Permissions:** Permanent world-accessible permissions established via `/etc/udev/rules.d/99-ion.rules` and early OpenRC initialization in `/etc/local.d/ion.start`.

### 3. Native 1080x2400 Display Geometry Patch
By default, `wlr_x11_output_create` in `libwlroots-0.20.so` clamped output windows to generic desktop dimensions.
- **Fix:** Binary patched opcode constants in `libwlroots-0.20.so` at offset `0x580d0` (`0x52812c02` = 2400, `0x52808701` = 1080) to spawn native 1080x2400 fullscreen presentation.
- **Cutout Offsets:** Calibrated `~/.config/ztmetro/layout.conf` with:
  - `STATUS_H=48`: Drops system indicators (battery, cellular, clock) cleanly below the Infinity-O front camera punch-hole.
  - `TILES_TOP_OFFSET=80`: Ensures Start screen tiles begin below the camera module.
  - `NAV_H=56`: Establishes comfortable finger targets for Back, ZRN (Home), and Search.
  - `SCALE=2.0`: Delivers authentic Windows Phone 8.1 tile and typography density.

### 4. Resolving GTK / Sandboxing Crashes on Linux 4.14
Modern GTK applications (such as Thunar) in Alpine edge rely on `glycin-loaders` for image decoding, which launches a Bubblewrap (`bwrap`) sandbox. Because downstream Android 4.14 kernels have unprivileged user namespaces disabled (`CONFIG_USER_NS`), `bwrap` exited with status 1 on every icon load, causing all GUI apps to abort with `ensure_surface_for_gicon` assertion errors.
- **Fix:** Disabled `glycin-loaders` and forced `gdk-pixbuf` to use standard, in-process `libpng` / `librsvg` decoders, restoring instantaneous application launching without sandboxing overhead.

### 5. Automated Console Login & Headless X11 Pipeline
- **Autologin:** Configured `agetty --autologin <user> tty1 linux` in `/etc/inittab`.
- **Auto-execution:** User login shell (`~/.bash_profile` / `~/.profile`) intercepts console login on `tty1` and executes `~/start.sh`.
- **Log Isolation:** `start.sh` starts `startx -- :0 vt1` and redirects `dbus-run-session ztmetro` output to `/tmp/ztmetro.log`, preventing kernel `fbcon` terminal glyphs from drawing over the live UI.

---

## Repository Structure

```text
A51_Project_Export/
├── pmaports/                          # postmarketOS / Nura package recipes
│   ├── device-samsung-a51/            # Deviceinfo, bootloader, and screen geometry
│   ├── linux-samsung-a51/             # Downstream 4.14.113 kernel config & patches
│   └── firmware-samsung-a51/          # Wi-Fi and Bluetooth firmware packaging
├── ztOS/                              # ztOS / ztMETRO Wayland compositor sources
│   ├── ztmetro.c                      # Main compositor code (sensor hub bypass, layouts)
│   ├── layout.conf                    # Galaxy A51 calibrated display parameters
│   └── Makefile                       # Build configuration
└── release/
    └── ztos-a51-release.tar.gz        # Pre-compiled one-click installer package
```

---

## Installation & Deployment Guide

### Phase 1: Flashing Base System via TWRP
1. Boot your Samsung Galaxy A51 into TWRP recovery (`Volume Up + Power` while connected to USB).
2. Go to **Wipe** -> **Advanced Wipe** and clear:
   - `Dalvik / ART Cache`
   - `Cache`
   - `System`
   - `Data`
3. Go to **Install**:
   - Tap **Install Image** in the lower right.
   - Flash `boot.img` to the **Boot** partition.
   - Flash `dtbo.img` to the **DTBO** partition.
   - Tap **Install Zip** and flash `postmarketos-samsung-a51.zip`.
4. Reboot the phone into the system (**Reboot -> System**).

### Phase 2: Deploying ztOS in One Command
Once the phone boots, connect it via USB to your PC. The phone will establish a network bridge at IP `172.16.42.1`.

1. From your PC terminal (Linux, macOS, or Windows PowerShell / WSL), copy the release bundle to the phone:
   ```bash
   scp -O ztos-a51-release.tar.gz nickolay@172.16.42.1:~
   ```

2. Log in via SSH:
   ```bash
   ssh nickolay@172.16.42.1
   ```

3. Unpack and run the automated installer:
   ```bash
   tar -xzvf ztos-a51-release.tar.gz
   cd ztos_release && sudo ./install.sh
   ```

The script will automatically install missing dependencies, disable conflicting services, set up ION video permissions, copy the pre-patched binaries, and reboot the device straight into ztOS.

---

## Usage & Shortcuts

- **Start Screen**: Swipe vertically to browse pinned live tiles. Tap and hold to enter tile edit mode.
- **App List**: Swipe left from Start to access the alphabetical application list.
- **Navigation Bar**:
  - `←` (Back): Returns to previous view or closes open app.
  - `ZRN` (Home): Returns directly to the Start tile grid.
  - `🔍` (Search): Opens quick application search.
- **Screen Sleep & Wake**: Press the hardware **Power** button to toggle screen off/on. AMOLED black turns display pixels completely off to conserve battery.
- **Capturing Screenshots**:
  - Immediate capture: `screenshot` (saves PNG to `~/Pictures/`).
  - Delayed capture: `screenshot 3` (3-second countdown to prepare the screen).

---

## Credits & License

- **ztOS / ztMETRO**: ztOS Mobile Project (Wayland / wlroots implementation).
- **postmarketOS / Nura**: postmarketOS community and Nura Wiki contributors for the initial `samsung-a51` device port.
- **Samsung A51 Adaptation & Optimization**: Ported, debugged, and optimized by Nickolay for the Samsung Galaxy A51 community.
- **License**: GPL-3.0 / MIT (see repository source headers).
