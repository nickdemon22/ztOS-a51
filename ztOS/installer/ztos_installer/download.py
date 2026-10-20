# SPDX-License-Identifier: GPL-2.0-or-later
"""Download prebuilt ztOS release images from GitHub."""
from __future__ import annotations

import json
import lzma
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

from .build import ROOT, WORK, Fail, say

REPO = "Zerone-Laboratories/ztOS"
API_URL = f"https://api.github.com/repos/{REPO}/releases/latest"


def fetch_release_info() -> dict:
    req = urllib.request.Request(API_URL, headers={"User-Agent": "ztOS-Installer"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise Fail(f"No published releases found yet on {REPO}.\n"
                       "Run 'Build image in this PC and install' to build locally.")
        raise Fail(f"GitHub API error ({e.code}): {e.reason}")
    except Exception as e:
        raise Fail(f"Failed to connect to GitHub ({API_URL}): {e}")


def download_file(url: str, dest: Path) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    say(f"downloading {dest.name}...")
    req = urllib.request.Request(url, headers={"User-Agent": "ztOS-Installer"})
    with urllib.request.urlopen(req) as resp, open(dest, "wb") as f:
        total = int(resp.headers.get("Content-Length", 0))
        done = 0
        while True:
            chunk = resp.read(1048576)  # 1 MB
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if total > 0:
                pct = int(done * 100 / total)
                bar = "█" * (pct // 5) + "░" * (20 - pct // 5)
                mb_done = done / (1024 * 1024)
                mb_total = total / (1024 * 1024)
                print(f"\rztos: [{bar}] {pct:3d}% ({mb_done:.1f} / {mb_total:.1f} MB)", end="", flush=True)
            else:
                mb_done = done / (1024 * 1024)
                print(f"\rztos: downloaded {mb_done:.1f} MB", end="", flush=True)
        print()
    return dest


def decompress_image(src: Path, dst: Path) -> Path:
    say(f"decompressing {src.name} -> {dst.name}...")
    dst.unlink(missing_ok=True)

    if src.suffix == ".zst":
        if shutil.which("zstd"):
            subprocess.run(["zstd", "-d", str(src), "-o", str(dst)], check=True)
            return dst
        # If no zstd command, fallback check if .xz also exists or error
        raise Fail("zstd not found on this system: sudo pacman -S zstd")

    if src.suffix == ".xz":
        with lzma.open(src, "rb") as fin, open(dst, "wb") as fout:
            while chunk := fin.read(4194304):
                fout.write(chunk)
        return dst

    raise Fail(f"unsupported compression: {src.name}")


def get_prebuilt(target: str) -> tuple[Path, Path | None]:
    """Downloads and prepares prebuilt images for target ('starlte' or 'qemu').
    Returns (root_image, boot_image_or_None)."""
    info = fetch_release_info()
    tag = info.get("tag_name", "latest")
    say(f"found ztOS release: {tag}")

    assets = {a["name"]: a["browser_download_url"] for a in info.get("assets", [])}
    has_zstd = shutil.which("zstd") is not None

    if target == "starlte":
        out_dir = WORK / "starlte"
        out_dir.mkdir(parents=True, exist_ok=True)

        # 1. Boot image
        boot_url = assets.get("ztos-starlte-boot.img") or assets.get("ztos-boot.img")
        if not boot_url:
            raise Fail("Release is missing ztos-starlte-boot.img")
        boot_path = ROOT / "out" / "ztos-boot.img"
        download_file(boot_url, boot_path)

        # 2. Root image (prefer .zst if zstd available, else .xz)
        root_zst = assets.get("ztos-starlte-root.img.zst")
        root_xz = assets.get("ztos-starlte-root.img.xz")

        if root_zst and has_zstd:
            archive = download_file(root_zst, out_dir / "root.img.zst")
        elif root_xz:
            archive = download_file(root_xz, out_dir / "root.img.xz")
        elif root_zst:
            archive = download_file(root_zst, out_dir / "root.img.zst")
        else:
            raise Fail("Release is missing ztos-starlte-root.img archive")

        root_img = out_dir / "root.img"
        decompress_image(archive, root_img)
        archive.unlink(missing_ok=True)
        return root_img, boot_path

    if target == "qemu":
        out_dir = WORK / "qemu"
        out_dir.mkdir(parents=True, exist_ok=True)

        qemu_zst = assets.get("ztos-qemu.img.zst")
        qemu_xz = assets.get("ztos-qemu.img.xz")

        if qemu_zst and has_zstd:
            archive = download_file(qemu_zst, out_dir / "ztos-qemu.img.zst")
        elif qemu_xz:
            archive = download_file(qemu_xz, out_dir / "ztos-qemu.img.xz")
        elif qemu_zst:
            archive = download_file(qemu_zst, out_dir / "ztos-qemu.img.zst")
        else:
            raise Fail("Release is missing ztos-qemu.img archive")

        disk_img = out_dir / "ztos-qemu.img"
        decompress_image(archive, disk_img)
        archive.unlink(missing_ok=True)
        return disk_img, None

    raise Fail(f"Unknown target: {target}")
