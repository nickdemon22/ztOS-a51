"""Touch-friendly output equalizer backed by EasyEffects' PipeWire engine."""
import json
import os
import socket
import subprocess
import time

import ztos
from PySide6.QtCore import QTimer


APP = ztos.App()
PRESET = "ztos-equalizer"
FREQUENCIES = (31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000)



class EasyEffects:
    """Client for EasyEffects' local control socket."""

    def __init__(self):
        runtime = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
        self.socket_path = os.path.join(runtime, "EasyEffectsServer")

    def ensure_running(self):
        if not os.path.exists(self.socket_path):
            subprocess.Popen(["easyeffects", "--service-mode", "--hide-window"],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        deadline = time.monotonic() + 8
        last_error = None
        while time.monotonic() < deadline:
            try:
                self.command("get_global_bypass", read_reply=True)
                return
            except OSError as exc:
                last_error = exc
                time.sleep(0.2)
        raise RuntimeError(f"EasyEffects did not start: {last_error}")

    def command(self, command, read_reply=False):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
            conn.settimeout(2)
            conn.connect(self.socket_path)
            conn.sendall((command + "\n").encode())
            return conn.recv(1024).decode().strip() if read_reply else ""

    @staticmethod
    def preset_data():
        bands = {}
        for index, frequency in enumerate(FREQUENCIES):
            bands[f"band{index}"] = {
                "frequency": float(frequency), "gain": 0.0, "mode": "RLC (BT)", "mute": False,
                "q": 1.0, "slope": "x1", "solo": False, "type": "Bell", "width": 4.0,
            }
        return {"output": {"blocklist": [], "equalizer#0": {
            "balance": 0.0, "bypass": False, "input-gain": 0.0, "left": dict(bands),
            "mode": "IIR", "num-bands": len(FREQUENCIES), "output-gain": 0.0,
            "right": dict(bands), "split-channels": False,
        }, "plugins_order": ["equalizer#0"]}}

    def activate_equalizer(self):
        data_home = os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share"))
        preset_dir = os.path.join(data_home, "easyeffects", "output")
        os.makedirs(preset_dir, exist_ok=True)
        with open(os.path.join(preset_dir, PRESET + ".json"), "w", encoding="utf-8") as f:
            json.dump(self.preset_data(), f, indent=2)
            f.write("\n")
        self.command(f"load_preset:output:{PRESET}")
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.command("get_last_loaded_preset:output", read_reply=True) == PRESET:
                return
            time.sleep(0.1)
        raise RuntimeError("EasyEffects did not activate the ztOS EQ preset")

    def set_gain(self, index, gain):
        value = f"{float(gain):.1f}"
        for channel in ("left", "right"):
            self.command(f"set_property:output:equalizer:0:{channel}:band{index}Gain:{value}")

    def set_bypass(self, bypass):
        self.command(f"global_bypass:{1 if bypass else 0}")


class EqualizerPage(ztos.Page):
    def __init__(self):
        super().__init__("output equalizer")
        self.store = ztos.store("settings")
        self.effects = EasyEffects()
        self.text("10-band system EQ", size=25)
        self.text("Applies to audio routed through PipeWire", size=13, color=ztos.GREY)
        self.space(8)
        self.status = self.text("Connecting to audio engine…", size=13, color=ztos.GREY)
        self.section("BANDS · dB")
        self.gains = list(self.store.get("gains", [0] * len(FREQUENCIES)))
        if len(self.gains) != len(FREQUENCIES):
            self.gains = [0] * len(FREQUENCIES)
        self.sliders = []
        self._pending = {}
        self._enabled = bool(self.store.get("enabled", True))
        for i, frequency in enumerate(FREQUENCIES):
            label = f"{frequency} Hz" if frequency < 1000 else f"{frequency / 1000:g} kHz"
            slider = self.slider(label, self.gains[i],
                on_change=lambda value, band=i: self._queue_gain(band, value),
                minimum=-12, maximum=12, suffix=" dB")
            self.sliders.append(slider)
        self.section("CONTROL")
        self.toggle("Equalizer enabled", on=self._enabled, on_change=self._set_enabled)
        self.entry("Flat", "Reset all bands", self.reset)
        self._connect_timer = QTimer(self, singleShot=True)
        self._connect_timer.timeout.connect(self._apply_pending)
        QTimer.singleShot(0, self._initialize)

    def _initialize(self):
        try:
            self.effects.ensure_running()
            self.effects.activate_equalizer()
            for index, gain in enumerate(self.gains):
                if gain:
                    self.effects.set_gain(index, gain)
            self.effects.set_bypass(not self._enabled)
            self.status.setText("EasyEffects connected · PipeWire output")
            self.status.setStyleSheet("color: #78d98b")
        except Exception as exc:
            self.status.setText(f"Audio engine unavailable: {exc}")

    def _queue_gain(self, index, gain):
        self.gains[index] = gain
        self._pending[index] = gain
        self.store["gains"] = list(self.gains)
        self._connect_timer.start(70)

    def _apply_pending(self):
        try:
            self.effects.ensure_running()
            for index, gain in self._pending.items():
                self.effects.set_gain(index, gain)
            self._pending.clear()
            self.store.save()
            self.status.setText("EasyEffects connected · PipeWire output")
        except Exception as exc:
            self.status.setText(f"Could not apply EQ: {exc}")

    def _set_enabled(self, enabled):
        self._enabled = enabled
        self.store["enabled"] = enabled
        self.store.save()
        try:
            self.effects.set_bypass(not enabled)
            self.status.setText("Equalizer enabled" if enabled else "Equalizer bypassed")
        except Exception as exc:
            self.status.setText(f"Could not change EQ state: {exc}")

    def reset(self):
        for index, slider in enumerate(self.sliders):
            slider.set_value(0)
            self._queue_gain(index, 0)


APP.run(EqualizerPage())
