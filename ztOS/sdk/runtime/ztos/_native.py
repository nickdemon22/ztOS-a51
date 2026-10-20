# SPDX-License-Identifier: GPL-2.0-or-later
"""Small ABI to the SDK's C renderer. All calls run on Qt's GL thread."""
import ctypes as C
import hashlib
import os
import pkgutil
import platform
import tempfile
from pathlib import Path


class NativeQuad(C.Structure):
    _fields_ = [("texture", C.c_uint32), ("src", C.c_float * 4),
                ("corners", C.c_float * 12), ("alpha", C.c_float)]


def _load():
    try:
        data = pkgutil.get_data("ztos", "_native_renderer.so")
    except (OSError, ValueError):
        data = None
    if data is None:
        # Source-tree runs use the host build, never a phone binary.
        path = Path(__file__).resolve().parents[2] / "native/prebuilt" / platform.machine() / "libztos-render.so"
        if not path.is_file():
            raise RuntimeError("build the SDK C renderer with: make -C sdk/native")
    else:
        digest = hashlib.sha256(data).hexdigest()
        root = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "ztos/native" / digest
        root.mkdir(parents=True, exist_ok=True, mode=0o700)
        path = root / "renderer.so"
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            fd, temporary = tempfile.mkstemp(dir=root, prefix=".renderer-")
            try:
                with os.fdopen(fd, "wb") as out:
                    out.write(data)
                os.replace(temporary, path)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
    lib = C.CDLL(str(path))
    lib.zsdk_abi.restype = C.c_uint
    if lib.zsdk_abi() != 1:
        raise RuntimeError("incompatible ztOS native renderer")
    lib.zsdk_create.restype = C.c_void_p
    lib.zsdk_destroy.argtypes = [C.c_void_p]
    lib.zsdk_max_texture.restype = C.c_uint
    lib.zsdk_upload.argtypes = [C.c_int, C.c_int, C.c_int, C.c_void_p]
    lib.zsdk_upload.restype = C.c_uint
    lib.zsdk_drop.argtypes = [C.c_uint]
    lib.zsdk_draw.argtypes = [C.c_void_p, C.c_float, C.c_float, C.c_int, C.c_int,
                              C.POINTER(NativeQuad), C.c_uint]
    lib.zsdk_turn.argtypes = [C.c_float] * 7 + [C.POINTER(C.c_float)]
    lib.zsdk_entrance.argtypes = [C.c_double, C.c_double, C.POINTER(C.c_double)]
    return lib


lib = _load()


def turn(w, h, y0, y1, angle, travel=0.0, depth=0.0):
    corners = (C.c_float * 12)()
    lib.zsdk_turn(w, h, y0, y1, angle, travel, depth, corners)
    return tuple(tuple(corners[i:i + 3]) for i in (0, 3, 6, 9))


def entrance(t, h):
    state = (C.c_double * 4)()
    lib.zsdk_entrance(t, h, state)
    return tuple(state)  # angle, depth, travel, opacity


class Renderer:
    def __init__(self):
        self.handle = lib.zsdk_create()
        if not self.handle:
            raise RuntimeError("native GLES initialization failed")

    def close(self):
        if self.handle:
            lib.zsdk_destroy(self.handle)
            self.handle = None

    def upload(self, image):
        bits = image.constBits()
        # from_buffer_copy is needed for Qt's read-only buffer; one copy per
        # content revision, never per frame. Keep its owner through the call.
        pixels = (C.c_ubyte * len(bits)).from_buffer_copy(bits)
        texture = lib.zsdk_upload(image.width(), image.height(), image.bytesPerLine(), pixels)
        if not texture:
            raise RuntimeError("native texture allocation failed")
        return texture

    def draw(self, quads, textures, w, h, dpr):
        batch = (NativeQuad * len(quads))()
        count = 0
        for quad in quads:
            texture = textures.get(quad.key)
            if texture is None or quad.alpha <= 0.01:
                continue
            item = batch[count]
            item.texture = texture
            item.src[:] = (quad.src.left(), quad.src.top(), quad.src.right(), quad.src.bottom())
            item.corners[:] = tuple(value for point in quad.corners for value in point)
            item.alpha = quad.alpha
            count += 1
        lib.zsdk_draw(self.handle, w, h, int(w * dpr), int(h * dpr), batch, count)
