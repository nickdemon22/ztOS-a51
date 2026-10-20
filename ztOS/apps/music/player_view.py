"""Now Playing view with white-in-black system audio equalizer, Avicii easter egg,
real-time SeekBar, and MDL2 controls.
"""
from typing import Optional, List
from PySide6.QtCore import Qt, QRectF, QPointF, QTimer
from PySide6.QtGui import QPainter, QColor, QPen, QBrush, QPainterPath
from PySide6.QtWidgets import QWidget, QSizePolicy

import ztos
from engine import PlayerEngine, CAVA_BARS
from data import is_saved

# Windows 10 Segoe MDL2 Assets glyphs
GLYPH_PLAY = "\uE768"
GLYPH_PAUSE = "\uE769"
GLYPH_PREV = "\uE892"
GLYPH_NEXT = "\uE893"
GLYPH_REPEAT = "\uE8EE"
GLYPH_SAVE = "\uE896"
GLYPH_SAVED = "\uE73E"
GLYPH_REPLAY = "\uE81C"
GLYPH_MUSIC = "\uEC4F"


class WhiteBarsEqualizer(QWidget):
    """Clean white bars in black equalizer listening to system PipeWire audio via Cava.
    Features an Avicii silhouette easter egg when Avicii tracks are playing.
    """

    def __init__(self, engine: PlayerEngine, parent=None):
        super().__init__(parent)
        self.engine = engine
        self._bars: List[float] = [0.0] * CAVA_BARS
        self._targets: List[float] = [0.0] * CAVA_BARS
        self.setFixedHeight(230)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

        self.engine.amplitudes_updated.connect(self._on_amplitudes)
        self._animation = QTimer(self)
        self._animation.setInterval(16)
        self._animation.timeout.connect(self._animate_bars)
        self._animation.start()

    def _on_amplitudes(self, raw_values: list):
        for i in range(min(len(self._targets), len(raw_values))):
            self._targets[i] = raw_values[i]

    def _animate_bars(self):
        # Animate independently of Cava's 25 FPS updates for steady motion.
        moving = False
        for i, target in enumerate(self._targets):
            current = self._bars[i]
            if target > current:
                current += (target - current) * 0.45
            else:
                current += (target - current) * 0.16
            if abs(current - target) < 0.003:
                current = target
            else:
                moving = True
            self._bars[i] = current
        if moving:
            self.update()

    def _is_avicii_playing(self) -> bool:
        t = self.engine.current_track
        if not t:
            return False
        meta = f"{t.get('artist', '')} {t.get('title', '')}".lower()
        return "avicii" in meta

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing)
        w, h = float(self.width()), float(self.height())

        p.fillRect(QRectF(0, 0, w, h), QColor(0, 0, 0))

        # White equalizer bars
        n = len(self._bars)
        pad_x = 18.0
        avail_w = w - (pad_x * 2.0)
        bar_gap = 3.0
        bar_w = max(2.0, (avail_w - (n - 1) * bar_gap) / n)
        max_bar_h = h - 28.0

        p.setPen(Qt.NoPen)
        p.setBrush(QColor(255, 255, 255))

        for i, val in enumerate(self._bars):
            # Dynamic height from real audio
            bh = max(3.0, val * max_bar_h)
            bx = pad_x + i * (bar_w + bar_gap)
            by = h - 14.0 - bh
            p.drawRect(QRectF(bx, by, bar_w, bh))

        # Easter Egg: Solid Black Avicii Logo in front of the white bars
        if self._is_avicii_playing():
            self._draw_avicii_logo(p, QRectF(0, 0, w, h))

    def _draw_avicii_logo(self, p: QPainter, rect: QRectF):
        cx, cy = rect.center().x(), rect.center().y() - 4.0
        # The AVICII mark uses two matching right triangles. Keep each triangle
        # at a 1:1 width-to-height ratio so the mark stays balanced at any size.
        side = 52.0
        gap = 8.0

        # Solid dark fill over the bars, with no outline.
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(0, 0, 0))

        # Left triangle points down and right.
        path_l = QPainterPath()
        path_l.moveTo(cx - gap / 2.0, cy - side / 2.0)
        path_l.lineTo(cx - gap / 2.0 - side, cy + side / 2.0)
        path_l.lineTo(cx - gap / 2.0, cy + side / 2.0)
        path_l.closeSubpath()
        p.drawPath(path_l)

        # Right triangle points up and left.
        path_r = QPainterPath()
        path_r.moveTo(cx + gap / 2.0, cy - side / 2.0)
        path_r.lineTo(cx + gap / 2.0 + side, cy - side / 2.0)
        path_r.lineTo(cx + gap / 2.0, cy + side / 2.0)
        path_r.closeSubpath()
        p.drawPath(path_r)


class NowPlayingPage(ztos.Page):
    """Full-screen Now Playing page with white-in-black system equalizer,
    SeekBar, and MDL2 player controls."""

    def __init__(self, engine: PlayerEngine):
        super().__init__("now playing")
        self.engine = engine

        # White-in-black Equalizer
        self.equalizer = self.add(WhiteBarsEqualizer(self.engine))
        self.space(12)

        # Status badge (Streaming vs Saved vs Buffering)
        self.lbl_badge = self.section("STREAMING")

        # Track title & artist
        self.lbl_title = self.text("No Track Selected", size=24)
        self.lbl_artist = self.text("Select a song to start streaming", size=14, color=ztos.GREY)
        self.space(16)

        # Seek Bar
        self.seekbar = self.add(ztos.SeekBar(0.0, 0.0, on_seek=self._on_user_seek))
        self.space(20)

        # Control buttons row (MDL2 symbols)
        self.btn_replay = ztos.RoundButton(GLYPH_REPLAY, "replay", on_tap=self._on_replay)
        self.btn_prev = ztos.RoundButton(GLYPH_PREV, "prev", on_tap=self._on_prev)
        self.btn_play = ztos.RoundButton(GLYPH_PLAY, "play", on_tap=self._on_play_toggle)
        self.btn_next = ztos.RoundButton(GLYPH_NEXT, "next", on_tap=self._on_next)
        self.btn_save = ztos.RoundButton(GLYPH_SAVE, "save", on_tap=self._on_save_toggle)

        self.round_buttons(
            self.btn_replay,
            self.btn_prev,
            self.btn_play,
            self.btn_next,
            self.btn_save,
        )

        # Connect engine signals
        self.engine.track_changed.connect(self._on_track_changed)
        self.engine.position_changed.connect(self._on_position_changed)
        self.engine.buffered_changed.connect(self._on_buffered_changed)
        self.engine.state_changed.connect(self._on_state_changed)
        self.engine.buffering_changed.connect(self._on_buffering_changed)

        if self.engine.current_track:
            self._on_track_changed(self.engine.current_track)
        self._on_state_changed(self.engine.is_playing)

    def _on_user_seek(self, seconds: float):
        self.engine.seek(seconds)

    def _on_replay(self):
        ztos.haptic("tap")
        self.engine.replay()

    def _on_prev(self):
        ztos.haptic("tap")
        self.engine.prev()

    def _on_play_toggle(self):
        ztos.haptic("tap")
        self.engine.toggle_pause()

    def _on_next(self):
        ztos.haptic("tap")
        self.engine.next()

    def _on_save_toggle(self):
        track = self.engine.current_track
        if not track:
            return
        ztos.haptic("tap")
        tid = track.get("id")
        if is_saved(tid):
            ztos.notify("Music", f"Already saved offline: {track.get('title')}")
            return

        self.btn_save.set_label("saving...")
        self.engine.download_track(
            track,
            on_progress=lambda tid, pct: None,
            on_finished=self._on_download_finished,
            on_failed=self._on_download_failed,
        )

    def _on_download_finished(self, track: dict, path: str):
        ztos.haptic("tick")
        ztos.notify("Saved Offline", track.get("title", "Song"))
        if self.engine.current_track and self.engine.current_track.get("id") == track.get("id"):
            self.btn_save.set_glyph(GLYPH_SAVED)
            self.btn_save.set_label("saved")
            self.lbl_badge.setText("SAVED OFFLINE")

    def _on_download_failed(self, tid: str, err: str):
        ztos.notify("Download Failed", err[:60])
        self.btn_save.set_glyph(GLYPH_SAVE)
        self.btn_save.set_label("save")

    def _on_track_changed(self, track: dict):
        title = track.get("title") or "Unknown"
        artist = track.get("artist") or "Unknown"
        self.lbl_title.setText(title)
        self.lbl_artist.setText(artist)

        saved = is_saved(track.get("id", ""))
        self.lbl_badge.setText("SAVED OFFLINE" if saved else "STREAMING")
        self.btn_save.set_glyph(GLYPH_SAVED if saved else GLYPH_SAVE)
        self.btn_save.set_label("saved" if saved else "save")

        dur = float(track.get("duration") or 0.0)
        self.seekbar.set_duration(dur)
        self.seekbar.set_position(0.0)
        self.equalizer.update()

    def _on_position_changed(self, pos: float, dur: float):
        self.seekbar.set_position(pos)
        if dur > 0:
            self.seekbar.set_duration(dur)

    def _on_buffered_changed(self, buf: float):
        self.seekbar.set_buffered(buf)

    def _on_state_changed(self, is_playing: bool):
        self.btn_play.set_glyph(GLYPH_PAUSE if is_playing else GLYPH_PLAY)
        self.btn_play.set_label("pause" if is_playing else "play")

    def _on_buffering_changed(self, is_buffering: bool):
        if is_buffering:
            self.lbl_badge.setText("BUFFERING STREAM...")
        else:
            saved = is_saved(self.engine.current_track.get("id", "") if self.engine.current_track else "")
            self.lbl_badge.setText("SAVED OFFLINE" if saved else "STREAMING")
