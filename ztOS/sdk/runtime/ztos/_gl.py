# SPDX-License-Identifier: GPL-2.0-or-later
"""The GPU layer: what moves is drawn by the GPU, as the phone's own apps do it.

Qt draws widgets on the CPU into the window's buffer and hands the whole buffer
to the shell for every frame: at 1440x2560 that is more than the phone's small
cores can do sixty times a second. So while a list scrolls, tabs slide or a
page turns, a window of our own (a Wayland subsurface, over the app) shows
pictures of the pages, made beforehand, and only says where each one goes;
the widgets under it are left alone until it is over.

A quad is (texture, source rect 0..1, four corners, alpha). The corners are
homogeneous (x, y, w) in points of the layer, so a page turned in perspective
is one quad, drawn exactly by the GPU.
"""
from __future__ import annotations

from typing import Callable

import sys

from PySide6.QtCore import QRect, QRectF, Qt, QTimer
from PySide6.QtGui import QImage, QSurfaceFormat, QOpenGLContext
from PySide6.QtWidgets import QWidget
from PySide6.QtOpenGL import QOpenGLWindow
from ._native import Renderer, lib


class Quad:
    __slots__ = ("key", "src", "corners", "alpha")

    def __init__(self, key, src: QRectF, corners, alpha: float = 1.0):
        self.key, self.src, self.corners, self.alpha = key, src, corners, alpha

    @staticmethod
    def rect(key, src: QRectF, x: float, y: float, w: float, h: float, alpha: float = 1.0) -> "Quad":
        return Quad(key, src, ((x, y, 1.0), (x + w, y, 1.0), (x, y + h, 1.0), (x + w, y + h, 1.0)), alpha)


class _Surface(QOpenGLWindow):
    def __init__(self, layer: "Layer"):
        super().__init__(QOpenGLWindow.NoPartialUpdate)
        self.layer = layer
        self.setFlags(Qt.FramelessWindowHint | Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus)
        fmt = QSurfaceFormat.defaultFormat()
        fmt.setAlphaBufferSize(8)       # motions that are only part of the screen
        fmt.setSwapInterval(1)          # (a lean, a dialog) blend over the app
        self.setFormat(fmt)
        self.renderer = None
        self.textures: dict = {}
        self.frameSwapped.connect(self._next)

    def initializeGL(self):
        try:
            self.renderer = Renderer()
            self.layer.max_texture = min(8192, int(lib.zsdk_max_texture()))
        except Exception as error:
            self.layer.broken = True
            print("ztos: native GPU renderer failed:", error, file=sys.stderr)
            return
        self.context().aboutToBeDestroyed.connect(self._cleanup)
        self.layer.initialized = True
        pending = dict(self.layer._pending_uploads)
        self.layer._pending_uploads.clear()
        for key, img in pending.items():
            self.layer.upload(key, img)

    def paintGL(self):
        if self.renderer is None or self.layer.broken:
            return
        if self.layer.warming:
            self.layer.warming = False
            if not self.layer.running:
                # Hiding from paintGL can race with the first motion. Let the
                # frame finish, then hide only if begin() has not claimed it.
                self.renderer.draw([], self.textures, max(1, self.width()), max(1, self.height()), self.devicePixelRatio())
                QTimer.singleShot(0, self.layer._hide_if_idle)
                return
        quads = []
        for _rect, frame in self.layer.sources.values():
            quads += frame() or []
        w = float(self.layer.host.width())
        h = float(self.layer.host.height())
        cw = float(self.layer.container.width())
        ch = float(self.layer.container.height())
        if cw > 1 and ch > 1 and (w <= 1 or h <= 1 or w != cw or h != ch):
            w, h = cw, ch
        if w <= 1 or h <= 1:
            return
        self.renderer.draw(quads, self.textures, w, h, self.devicePixelRatio())

    def _cleanup(self):
        self.makeCurrent()
        for texture in self.textures.values():
            lib.zsdk_drop(texture)
        self.textures.clear()
        if self.renderer is not None:
            self.renderer.close()
            self.renderer = None
        self.layer.initialized = False
        self.doneCurrent()

    def _next(self):
        # A Wayland subsurface (which this is) is synchronised with its parent:
        # its new frame is put on the screen only when the window under it
        # commits too. So the window commits with each frame of ours: one
        # pixel of it repainted, under this layer, nothing to see.
        if self.layer.running:
            self.layer.host.update(0, 0, 1, 1)




class Layer:
    """One per app window. begin(owner, rect, frame) puts a motion on the GPU:
    from then on the layer draws that frame() (a list of Quads, in points of
    the whole window) every display frame, over the app, until end(owner).
    Motions coexist -- a dialog swinging while a pressed tile leans -- and the
    layer composes them all; what is under it is left alone until it is over."""

    def __init__(self, host: QWidget):
        self.host = host
        self.broken = False
        self.max_texture = 4096
        self.surface = _Surface(self)
        self.container = QWidget.createWindowContainer(self.surface, host)
        self.container.setAttribute(Qt.WA_TransparentForMouseEvents)
        self.container.setFocusPolicy(Qt.NoFocus)
        self.container.hide()
        self.initialized = False
        self.warming = False
        self.pending = False
        self._pending_uploads: dict = {}
        self.sources: dict = {}     # owner -> (rect, frame)

    @property
    def running(self) -> bool:
        return bool(self.sources)

    def warm(self):
        """Make the GPU side now (Qt makes it when the surface first shows):
        one black pixel, in the app's own black corner, for a frame."""
        if self.initialized or self.broken:
            return
        self.warming = True
        # Context creation needs a mapped surface, not a full-screen overlay.
        # begin() resizes it to the host before the first animated frame.
        self.container.setGeometry(QRect(0, 0, 1, 1))
        self.surface.resize(1, 1)
        self.container.show()
        self.surface.update()

    def _hide_if_idle(self):
        if not self.sources:
            self.container.hide()

    def upload(self, key, image: QImage):
        """A picture for the quads to show, by key (replacing one there)."""
        if not self.initialized:
            self._pending_uploads[key] = image
            return
        was_current = QOpenGLContext.currentContext() == self.surface.context()
        if not was_current:
            self.surface.makeCurrent()
        try:
            converted = image.convertToFormat(QImage.Format_RGBA8888_Premultiplied)
            texture = self.surface.renderer.upload(converted)
            old = self.surface.textures.get(key)
            self.surface.textures[key] = texture
            if old is not None:
                lib.zsdk_drop(old)
        finally:
            if not was_current:
                self.surface.doneCurrent()

    def drop(self, key):
        self._pending_uploads.pop(key, None)
        texture = self.surface.textures.pop(key, None)
        if texture is not None and self.initialized:
            was_current = QOpenGLContext.currentContext() == self.surface.context()
            if not was_current:
                self.surface.makeCurrent()
            try:
                lib.zsdk_drop(texture)
            finally:
                if not was_current:
                    self.surface.doneCurrent()

    def has(self, key) -> bool:
        return key in self.surface.textures

    def begin(self, owner, rect, frame: Callable[[], list]) -> bool:
        """Put `owner`'s motion on the GPU (it may already be there). The frame's
        quads are in points of the window, whatever rect is given (it says what
        the motion is of)."""
        new = not self.sources
        self.sources[owner] = (QRectF(rect), frame)
        if new:
            from . import _settle
            _settle()           # Qt repaints what is under it: not a change of the page
            self.warming = False
            self.container.setGeometry(self.host.rect())
            self.surface.resize(self.host.size())
            self.container.show()
            self.container.raise_()
            self.surface.update()
            # a subsurface (shown again, or new) appears when its parent commits:
            # the window commits now, and with every frame asked for (update)
            self.host.update(0, 0, 1, 1)
        else:
            self.refresh()
        return True

    def end(self, owner):
        """The motion is over. The layer goes when the last one does."""
        if owner not in self.sources:
            return
        del self.sources[owner]
        if self.sources:
            self.refresh()
            return
        from . import _settle
        _settle()               # nor is what it uncovers
        self.container.hide()
        self.host.update()

    def update(self):
        """Something moved: draw a frame."""
        self.refresh()

    def refresh(self):
        """Something moved: draw a frame."""
        if self.sources:
            self.surface.update()
            self.host.update(0, 0, 1, 1)    # (the parent commits too: see begin)
