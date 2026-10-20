"""Updates: ztOS's software updater. What can be updated, and installing it;
the work is done by the system (ztos-update, as root, started through systemd),
this shows how it goes."""
import subprocess
import ztos
from PySide6.QtCore import QTimer

app = ztos.App()
STATUS = "/run/ztos-update/status"
LIST = "/var/lib/ztos/updates"


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def start(unit):
    """Ask systemd to run one of the updater's services (allowed without a password)."""
    try:
        subprocess.Popen(["systemctl", "start", "--no-block", unit])
        return True
    except OSError:
        return False


class Home(ztos.Page):
    def __init__(self):
        super().__init__("updates")
        self.state = self.text("", size=17)
        self.progress = self.add(ztos.ProgressBar(0, caption=""))
        self.progress.hide()
        self.space(12)
        self.check_entry = self.entry("check for updates", "look now", self.check)
        self.install_entry = self.entry("install updates", "", self.install)
        self.section("what can be updated")
        self.packages = self.text("", size=13, color=ztos.GREY)
        self.timer = QTimer(self, interval=700)
        self.timer.timeout.connect(self.refresh)

    def shown(self):
        self.refresh()
        self.timer.start()

    def check(self):
        if start("ztos-update-check.service"):
            self.busy(True)

    def install(self):
        ztos.message("Install updates?", "ztOS will download and install every update. You can keep using the "
                     "phone; some apps may need opening again afterwards.", ["install", "cancel"],
                     lambda choice: choice == 0 and start("ztos-update-apply.service"))

    def refresh(self):
        words = read(STATUS).split() or ["idle"]
        state = words[0]
        updates = [line for line in read(LIST).splitlines() if line.strip()]
        working = state in ("checking", "downloading", "installing")
        self.busy(working)
        self.check_entry.setVisible(not working)
        self.install_entry.setVisible(bool(updates) and not working)
        self.install_entry.set_detail(f"{len(updates)} waiting")
        self.progress.setVisible(state in ("downloading", "installing"))
        if state == "installing" and len(words) >= 3:
            k, n = int(words[1]), max(1, int(words[2]))
            self.progress.set_value(100 * k / n)
            text = f"Installing {k} of {n}: {words[3] if len(words) > 3 else ''}"
        elif state == "downloading":
            self.progress.set_value(None)
            text = "Downloading the updates..."
        elif state == "checking":
            text = "Looking for updates..."
        elif state == "available":
            text = f"{len(updates)} updates can be installed."
        elif state == "uptodate":
            text = "ztOS is up to date."
        elif state == "done":
            text = f"Updated: {words[1] if len(words) > 1 else ''} updates installed."
        elif state == "failed":
            text = "That did not work. The details are in /var/log/ztos-update.log."
        else:
            text = "Check to see if there is anything new."
        self.state.setText(text)
        self.packages.setText("\n".join(updates[:60]) + ("\n..." if len(updates) > 60 else "") if updates
                              else "Nothing waiting.")


app.run(Home())
