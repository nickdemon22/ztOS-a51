"""High-performance audio engine with buffered streaming, non-blocking mpv IPC,
Cava playback-monitor visualizer, and stream URL pre-fetching.
"""
import os
import sys
import time
import json
import socket
import tempfile
import threading
import subprocess
from typing import Optional, List, Dict
from PySide6.QtCore import QObject, Signal, QTimer, QThread

from data import (
    ensure_music_dir,
    get_local_file,
    is_saved,
    save_track_meta,
    add_to_history,
)

SOCKET_PATH = "/tmp/ztos-music-mpv.sock"
CAVA_BARS = 24


class CavaVisualizerListener(threading.Thread):
    """Passively monitor existing playback without keeping the speaker sink active."""

    def __init__(self, callback):
        super().__init__(daemon=True)
        self.callback = callback
        self.running = True
        self._proc: Optional[subprocess.Popen] = None
        self._cfg_path: Optional[str] = None

    def run(self):
        conf = f"""
[general]
bars = {CAVA_BARS}
framerate = 25

[input]
# Cava defaults to always processing, which drives the S9 speaker sink even
# when music is paused. Only observe playback that is already running.
method = pipewire
source = auto
active = 0
sample_rate = 48000

[output]
method = raw
raw_target = /dev/stdout
data_format = binary
bit_format = 8bit
"""
        try:
            with tempfile.NamedTemporaryFile("w", delete=False) as f:
                f.write(conf)
                self._cfg_path = f.name

            self._proc = subprocess.Popen(
                ["cava", "-p", self._cfg_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                # Override Cava's hard-coded 512-frame latency request. The
                # S9 playback path needs the usual 2048-frame music buffer.
                env={**os.environ, "PIPEWIRE_LATENCY": "2048/48000"},
            )
            frame_size = CAVA_BARS
            pending = bytearray()
            while self.running and self._proc.poll() is None:
                raw = self._proc.stdout.read1(frame_size * 4)
                if not raw:
                    time.sleep(0.01)
                    continue
                pending.extend(raw)
                while len(pending) >= frame_size:
                    frame = pending[:frame_size]
                    del pending[:frame_size]
                    # Normalize 0..255 to 0.0..1.0
                    values = [b / 255.0 for b in frame]
                    self.callback(values)
        except Exception:
            pass
        finally:
            self.stop()

    def stop(self):
        self.running = False
        if self._proc:
            try:
                self._proc.terminate()
            except Exception:
                pass
            self._proc = None
        if self._cfg_path and os.path.exists(self._cfg_path):
            try:
                os.unlink(self._cfg_path)
            except Exception:
                pass
            self._cfg_path = None


class SearchWorker(QThread):
    results_ready = Signal(list)
    search_failed = Signal(str)

    def __init__(self, query: str, parent=None):
        super().__init__(parent)
        self.query = query

    def run(self):
        try:
            import yt_dlp
            opts = {
                "quiet": True,
                "extract_flat": True,
                "skip_download": True,
                "no_warnings": True,
            }
            with yt_dlp.YoutubeDL(opts) as ydl:
                res = ydl.extract_info(f"ytsearch8:{self.query}", download=False)
                entries = res.get("entries", [])
                results = []
                for e in entries:
                    if not e:
                        continue
                    results.append({
                        "id": e.get("id"),
                        "title": e.get("title") or "Unknown Title",
                        "artist": e.get("uploader") or e.get("channel") or "Unknown Artist",
                        "duration": float(e.get("duration") or 0.0),
                        "category": "search",
                    })
                self.results_ready.emit(results)
        except Exception as exc:
            self.search_failed.emit(str(exc))


class DownloadWorker(QThread):
    progress = Signal(str, float)
    finished = Signal(dict, str)
    failed = Signal(str, str)

    def __init__(self, track: dict, parent=None):
        super().__init__(parent)
        self.track = track

    def run(self):
        track_id = self.track.get("id", "")
        if not track_id:
            self.failed.emit("", "Invalid track ID")
            return

        dest_dir = ensure_music_dir()
        out_tmpl = os.path.join(dest_dir, f"{track_id}.%(ext)s")

        def hook(d):
            if d.get("status") == "downloading":
                total = d.get("total_bytes") or d.get("total_bytes_estimate") or 0
                downloaded = d.get("downloaded_bytes") or 0
                if total > 0:
                    pct = (downloaded / total) * 100.0
                    self.progress.emit(track_id, pct)
            elif d.get("status") == "finished":
                self.progress.emit(track_id, 100.0)

        try:
            import yt_dlp
            opts = {
                "format": "bestaudio/best",
                "outtmpl": out_tmpl,
                "quiet": True,
                "no_warnings": True,
                "progress_hooks": [hook],
            }
            with yt_dlp.YoutubeDL(opts) as ydl:
                ydl.download([f"https://www.youtube.com/watch?v={track_id}"])

            fpath = get_local_file(track_id)
            if fpath:
                save_track_meta(self.track, fpath)
                self.finished.emit(self.track, fpath)
            else:
                self.failed.emit(track_id, "File not found after download")
        except Exception as exc:
            self.failed.emit(track_id, str(exc))


class StreamResolverWorker(QThread):
    resolved = Signal(dict, str)     # track, stream_url
    failed = Signal(dict, str)

    def __init__(self, track: dict, parent=None):
        super().__init__(parent)
        self.track = track

    def run(self):
        tid = self.track.get("id")
        try:
            import yt_dlp
            opts = {
                "format": "bestaudio/best",
                "quiet": True,
                "no_warnings": True,
                "skip_download": True,
            }
            with yt_dlp.YoutubeDL(opts) as ydl:
                info = ydl.extract_info(f"https://www.youtube.com/watch?v={tid}", download=False)
                url = info.get("url")
                if url:
                    self.resolved.emit(self.track, url)
                else:
                    self.failed.emit(self.track, "No stream URL found")
        except Exception as exc:
            self.failed.emit(self.track, str(exc))


class PlayerEngine(QObject):
    position_changed = Signal(float, float)      # (pos, duration)
    buffered_changed = Signal(float)             # buffered seconds
    state_changed = Signal(bool)                 # is_playing
    track_changed = Signal(dict)                 # current track dict
    queue_changed = Signal()
    buffering_changed = Signal(bool)             # is_buffering
    amplitudes_updated = Signal(list)            # real-time FFT bars

    def __init__(self, parent=None):
        super().__init__(parent)
        self._proc: Optional[subprocess.Popen] = None
        self._sock: Optional[socket.socket] = None
        self._sock_lock = threading.Lock()
        self._reader_running = False

        self.current_track: Optional[dict] = None
        self.queue: List[dict] = []
        self.queue_index: int = 0
        self.is_playing: bool = False
        self.is_buffering: bool = False
        self.position: float = 0.0
        self.duration: float = 0.0
        self.buffered: float = 0.0
        self.repeat_mode: str = "all"

        self._active_downloads: Dict[str, DownloadWorker] = {}
        self._active_search: Optional[SearchWorker] = None
        self._active_resolver: Optional[StreamResolverWorker] = None
        self._url_cache: Dict[str, tuple[str, float]] = {}  # id -> (url, expire_time)

        self._start_mpv()
        self._start_cava()

    def _start_mpv(self):
        """Starts mpv with buffered streaming and socket IPC."""
        try:
            subprocess.run(["killall", "-q", "mpv"], check=False)
            time.sleep(0.05)
        except Exception:
            pass

        if os.path.exists(SOCKET_PATH):
            try:
                os.unlink(SOCKET_PATH)
            except OSError:
                pass

        cmd = [
            "mpv",
            "--idle=yes",
            "--no-video",
            f"--input-ipc-server={SOCKET_PATH}",
            "--ao=pipewire,pulse,alsa",
            "--demuxer-readahead-secs=120",
            "--demuxer-max-bytes=64M",
            "--demuxer-max-back-bytes=32M",
            "--cache=yes",
            "--cache-secs=120",
            "--force-seekable=yes",
            "--ytdl-format=bestaudio/best",
        ]
        try:
            self._proc = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception as exc:
            print("Failed to start mpv:", exc)
            return

        for _ in range(25):
            if os.path.exists(SOCKET_PATH):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(SOCKET_PATH)
                    self._sock = s
                    break
                except (OSError, socket.error):
                    time.sleep(0.08)
            time.sleep(0.08)

        if self._sock:
            self._start_ipc_reader()

    def _start_ipc_reader(self):
        """Dedicated background reader thread: listens for observe_property pushes."""
        # Setup property observers
        self._send_raw(["observe_property", 1, "time-pos"])
        self._send_raw(["observe_property", 2, "duration"])
        self._send_raw(["observe_property", 3, "pause"])
        self._send_raw(["observe_property", 4, "demuxer-cache-duration"])
        self._send_raw(["observe_property", 5, "idle-active"])

        self._reader_running = True

        def reader():
            sock_file = self._sock.makefile("r", buffering=1, errors="ignore")
            while self._reader_running and self._sock:
                try:
                    line = sock_file.readline()
                    if not line:
                        break
                    line = line.strip()
                    if not line:
                        continue
                    msg = json.loads(line)
                    event = msg.get("event")
                    if event == "property-change":
                        name = msg.get("name")
                        val = msg.get("data")
                        if name == "time-pos" and val is not None:
                            self.position = float(val)
                            self.position_changed.emit(self.position, self.duration)
                        elif name == "duration" and val is not None:
                            self.duration = float(val)
                            self.position_changed.emit(self.position, self.duration)
                        elif name == "pause":
                            playing = not bool(val)
                            if self.is_playing != playing:
                                self.is_playing = playing
                                self.state_changed.emit(self.is_playing)
                        elif name == "demuxer-cache-duration" and val is not None:
                            self.buffered = self.position + float(val)
                            self.buffered_changed.emit(self.buffered)
                        elif name == "idle-active":
                            if val is True and self.duration > 0 and self.position >= (self.duration - 2.0):
                                self._handle_track_finished()
                    elif event == "playback-restart":
                        self.is_buffering = False
                        self.buffering_changed.emit(False)
                        self.is_playing = True
                        self.state_changed.emit(True)
                except Exception:
                    pass

        t = threading.Thread(target=reader, daemon=True)
        t.start()

    def _send_raw(self, command: list):
        with self._sock_lock:
            if not self._sock:
                return
            try:
                msg = json.dumps({"command": command}) + "\n"
                self._sock.sendall(msg.encode())
            except Exception:
                pass

    def _start_cava(self):
        self._cava = CavaVisualizerListener(self._on_cava_amplitudes)
        self._cava.start()

    def _on_cava_amplitudes(self, values: list):
        self.amplitudes_updated.emit(values)

    def _handle_track_finished(self):
        if self.repeat_mode == "one":
            self.replay()
        elif self.repeat_mode == "all":
            self.next()
        else:
            self.is_playing = False
            self.state_changed.emit(False)

    def play_track(self, track: dict, playlist: Optional[List[dict]] = None):
        """Loads and begins playing a track with buffered streaming."""
        if playlist is not None:
            self.queue = list(playlist)
            try:
                self.queue_index = next(
                    i for i, t in enumerate(self.queue) if t.get("id") == track.get("id")
                )
            except StopIteration:
                self.queue.insert(0, track)
                self.queue_index = 0
            self.queue_changed.emit()

        self.current_track = track
        self.position = 0.0
        self.duration = float(track.get("duration") or 0.0)
        self.buffered = 0.0
        self.track_changed.emit(track)
        self.position_changed.emit(0.0, self.duration)
        add_to_history(track)

        # 1. Offline file
        local = get_local_file(track.get("id", ""))
        if local:
            track["is_saved"] = True
            track["local_path"] = local
            self._load_source(local)
            self._prefetch_next()
            return

        track["is_saved"] = False

        # 2. Check cached direct stream URL
        cached_url, expire = self._url_cache.get(track.get("id", ""), ("", 0))
        if cached_url and time.time() < expire:
            self._load_source(cached_url)
            self._prefetch_next()
            return

        # 3. Resolve stream URL in background, or fall back to ytdl stream
        self.is_buffering = True
        self.buffering_changed.emit(True)

        def on_resolved(tr, stream_url):
            if self.current_track and self.current_track.get("id") == tr.get("id"):
                self._url_cache[tr["id"]] = (stream_url, time.time() + 14400)
                self._load_source(stream_url)
                self._prefetch_next()

        def on_failed(tr, err):
            if self.current_track and self.current_track.get("id") == tr.get("id"):
                # Fallback to direct mpv ytdl hook
                source = f"https://www.youtube.com/watch?v={tr.get('id')}"
                self._load_source(source)

        if self._active_resolver and self._active_resolver.isRunning():
            self._active_resolver.terminate()
            self._active_resolver.wait()

        self._active_resolver = StreamResolverWorker(track, self)
        self._active_resolver.resolved.connect(on_resolved)
        self._active_resolver.failed.connect(on_failed)
        self._active_resolver.start()

    def _load_source(self, source: str):
        self._send_raw(["loadfile", source, "replace"])
        self._send_raw(["set_property", "pause", False])
        self.is_playing = True
        self.state_changed.emit(True)

    def _prefetch_next(self):
        """Pre-fetches stream URL for the upcoming track in the queue."""
        if not self.queue or len(self.queue) <= 1:
            return
        next_idx = (self.queue_index + 1) % len(self.queue)
        nxt = self.queue[next_idx]
        nid = nxt.get("id")
        if not nid or is_saved(nid) or nid in self._url_cache:
            return

        def _do_prefetch():
            try:
                import yt_dlp
                opts = {"format": "bestaudio/best", "quiet": True, "no_warnings": True, "skip_download": True}
                with yt_dlp.YoutubeDL(opts) as ydl:
                    info = ydl.extract_info(f"https://www.youtube.com/watch?v={nid}", download=False)
                    u = info.get("url")
                    if u:
                        self._url_cache[nid] = (u, time.time() + 14400)
            except Exception:
                pass

        threading.Thread(target=_do_prefetch, daemon=True).start()

    def toggle_pause(self):
        if not self.current_track:
            if self.queue:
                self.play_track(self.queue[0])
            return
        self._send_raw(["cycle", "pause"])

    def seek(self, seconds: float):
        if not self.current_track:
            return
        seconds = max(0.0, float(seconds))
        self.position = seconds
        # Streaming sources can take a long time to rebuild their precise
        # decoder position after an HR seek. A keyframe seek is immediate and
        # avoids provoking long audio graph reconfiguration on the phone.
        self._send_raw(["seek", seconds, "absolute+keyframes"])
        self.position_changed.emit(self.position, self.duration)

    def replay(self):
        if self.current_track:
            self.seek(0.0)
            self._send_raw(["set_property", "pause", False])
            self.is_playing = True
            self.state_changed.emit(True)

    def next(self):
        if not self.queue:
            return
        self.queue_index = (self.queue_index + 1) % len(self.queue)
        self.play_track(self.queue[self.queue_index])

    def prev(self):
        if self.position > 3.0:
            self.replay()
            return
        if not self.queue:
            return
        self.queue_index = (self.queue_index - 1) % len(self.queue)
        self.play_track(self.queue[self.queue_index])

    def cycle_repeat(self) -> str:
        modes = ["all", "one", "off"]
        idx = (modes.index(self.repeat_mode) + 1) % len(modes)
        self.repeat_mode = modes[idx]
        return self.repeat_mode

    def download_track(self, track: dict, on_progress=None, on_finished=None, on_failed=None):
        tid = track.get("id")
        if not tid or tid in self._active_downloads:
            return

        worker = DownloadWorker(track, self)
        if on_progress:
            worker.progress.connect(on_progress)
        if on_finished:
            worker.finished.connect(on_finished)
        if on_failed:
            worker.failed.connect(on_failed)

        def _cleanup(*_):
            self._active_downloads.pop(tid, None)

        worker.finished.connect(_cleanup)
        worker.failed.connect(_cleanup)

        self._active_downloads[tid] = worker
        worker.start()

    def search(self, query: str, on_results=None, on_error=None):
        if self._active_search and self._active_search.isRunning():
            self._active_search.terminate()
            self._active_search.wait()

        worker = SearchWorker(query, self)
        if on_results:
            worker.results_ready.connect(on_results)
        if on_error:
            worker.search_failed.connect(on_error)

        self._active_search = worker
        worker.start()

    def close(self):
        self._reader_running = False
        if self._cava:
            self._cava.stop()
        if self._sock:
            try:
                self._send_raw(["quit"])
                self._sock.close()
            except OSError:
                pass
        if self._proc:
            try:
                self._proc.terminate()
            except OSError:
                pass
