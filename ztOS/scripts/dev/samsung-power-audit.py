#!/usr/bin/env python3
"""Read power policy from the boot image in a Samsung AP firmware ZIP.

Requires lz4 and dtc. Extracts only boot.img.lz4 into a temporary directory;
never flashes or mounts a firmware partition. Prints a JSON evidence report.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import zipfile


def block(text, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\{", text)
    if not match:
        raise ValueError(f"missing DT node {name}")
    start, pos, depth = match.end(), match.end(), 1
    while depth and pos < len(text):
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    if depth:
        raise ValueError(f"unterminated DT node {name}")
    return text[start:pos - 1]


def cells(text, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*=\s*<([^>]+)>", text)
    if not match:
        raise ValueError(f"missing DT property {name}")
    return [int(value, 0) for value in match.group(1).split()]


def audit(path):
    with tempfile.TemporaryDirectory(prefix="ztos-firmware-") as tmp:
        tmp = Path(tmp)
        with zipfile.ZipFile(path) as archive:
            ap = next(n for n in archive.namelist() if "/AP_" in n)
            with archive.open(ap) as stream, tarfile.open(fileobj=stream, mode="r|") as tar:
                for member in tar:
                    if Path(member.name).name == "boot.img.lz4":
                        with (tmp / "boot.lz4").open("wb") as output:
                            shutil.copyfileobj(tar.extractfile(member), output)
                        break
                else:
                    raise ValueError("AP archive has no boot.img.lz4")
        subprocess.run(["lz4", "-d", "-f", str(tmp / "boot.lz4"),
                        str(tmp / "boot.img")], check=True, capture_output=True)
        image = (tmp / "boot.img").read_bytes()
        if image[:8] != b"ANDROID!":
            raise ValueError("boot image has no Android header")
        kernel_size = struct.unpack_from("<I", image, 8)[0]
        page_size = struct.unpack_from("<I", image, 36)[0]
        if page_size < 512 or page_size & (page_size - 1):
            raise ValueError("invalid boot image page size")
        kernel = image[page_size:page_size + kernel_size]
        report = {"firmware": str(path), "ap_member": ap,
                  "boot_sha256": hashlib.sha256(image).hexdigest(),
                  "kernel_sha256": hashlib.sha256(kernel).hexdigest(), "boards": []}
        offset = image.find(b"DTBH")
        if offset < 0:
            raise ValueError("Samsung DTBH table missing")
        while True:
            offset = image.find(bytes.fromhex("d00dfeed"), offset + 4)
            if offset < 0:
                break
            if offset + 40 > len(image):
                continue
            size = struct.unpack_from(">I", image, offset + 4)[0]
            if size < 40 or offset + size > len(image):
                continue
            (tmp / "board.dtb").write_bytes(image[offset:offset + size])
            dts = subprocess.run(["dtc", "-I", "dtb", "-O", "dts",
                                  str(tmp / "board.dtb")], check=True,
                                 capture_output=True, text=True).stdout
            power = block(dts, "exynos-powermode")
            refs = cells(block(power, "SYS_SICD"), "ref-idle-ip")
            masks = [0xffffffff] * 4
            for index in refs:
                masks[index // 32] &= ~(1 << (index % 32))
            wake = block(power, "wakeup-masks")
            wake_masks = {}
            for name in ["wakeup-mask", "wakeup-mask2", "wakeup-mask3", "wakeup-mask4"]:
                node = block(wake, name)
                wake_masks[hex(cells(node, "reg-offset")[0])] = hex(cells(node, "mask")[0])
            report["boards"].append({
                "model": re.search(r'\bmodel = "([^"]+)"', dts).group(1),
                "sicd_residency_us": cells(power, "sicd_residency")[0],
                "cpd_residency_us": cells(power, "cpd_residency")[0],
                "sicd_enabled": bool(cells(power, "sicd_enabled")[0]),
                "cpd_enabled": bool(cells(power, "cpd_enabled")[0]),
                "sicd_idle_ip_masks": [hex(v) for v in masks],
                "sicd_wakeup_masks": wake_masks,
                "psci_cpu_suspend": hex(cells(block(dts, "psci"), "cpu_suspend")[0]),
            })
        if not report["boards"]:
            raise ValueError("no valid Samsung board device trees")
        return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware_zip", type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.firmware_zip), indent=2))
