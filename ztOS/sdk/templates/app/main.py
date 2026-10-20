"""@NAME@: a ztOS app. Try it with `ztos-sdk run`, put it on the phone with `ztos-sdk deploy`."""
import ztos
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QPushButton

app = ztos.App()


class Home(ztos.Pivot):
    """Two pages side by side: swipe between them, or tap the grey title."""

    def __init__(self):
        main = ztos.Page("home")
        main.text("This is your app. Everything here is ordinary Qt: change it in main.py, "
                  "or draw pages in Qt Designer (form.ui).")
        self.count = 0
        self.counter = main.entry("count", "tapped 0 times", self.tapped, on_hold=self.held)
        main.toggle("Lights", on=True, on_change=lambda on: print("lights", on))
        main.slider("Brightness", 60, on_change=lambda v: None)
        main.entry("a page drawn in Designer", "form.ui", lambda: app.push(DesignedPage()))
        main.entry("a deeper page", "it swings in like the phone's own", lambda: app.push(Details()))

        settings = ztos.Page("settings")
        self.store = ztos.store()           # kept on the phone between runs
        settings.toggle("Notifications", on=self.store.get("notify", False), on_change=self.notifications)
        settings.textbox("Your name", text=self.store.get("name", ""), placeholder="type here",
                         on_submit=self.named)
        self.sync = settings.add(ztos.ProgressBar(0, caption="Sync"))
        settings.space(16)
        settings.entry("sync now", "a progress bar that glides", self.sync_now)
        settings.entry("delete everything", "a message from the top", self.ask)
        super().__init__([main, settings])

    def named(self, name):
        self.store["name"] = name
        self.store.save()
        ztos.notify("Hello", name)

    def sync_now(self):
        self.pages[1].busy(True)            # the dots across the top while it works
        steps = iter((25, 60, 90, 100))

        def step():
            value = next(steps, None)
            if value is None:
                self.pages[1].busy(False)
                return
            self.sync.set_value(value)
            QTimer.singleShot(600, step)
        step()

    def ask(self):
        ztos.message("Delete everything?", "Your notes can't be brought back afterwards.",
                     ["delete", "cancel"], lambda choice: print("chose", choice))

    def tapped(self):
        self.count += 1
        self.counter.set_detail(f"tapped {self.count} times")

    def held(self):
        ztos.menu(self.counter, [("reset", self.reset), ("add ten", lambda: self.add(10))])

    def reset(self):
        self.count = -1
        self.tapped()

    def add(self, n):
        self.count += n - 1
        self.tapped()

    def notifications(self, on):
        self.store["notify"] = on
        self.store.save()
        if on:
            ztos.notify("@NAME@", "Notifications are on")


class Details(ztos.Page):
    def __init__(self):
        super().__init__("details")
        self.text("Back (or Escape on the laptop) swings this page away again.")
        self.section("round buttons")
        self.round_buttons(ztos.RoundButton("add", "add", lambda: ztos.haptic("tap")),
                           ztos.RoundButton("refresh", "refresh"),
                           ztos.RoundButton("delete", "delete"))
        self.bar(ztos.RoundButton("check", "done", app.pop), ztos.RoundButton("cancel", "cancel", app.pop))


class DesignedPage(ztos.Page):
    """A page whose body was drawn in Qt Designer: `ztos-sdk designer` opens it."""

    def __init__(self):
        super().__init__("designed")
        form = ztos.load_ui("form.ui")
        self.add(form)
        form.findChild(QPushButton, "greetButton").clicked.connect(
            lambda: form.ui.greeting.setText(f"Hello, {form.ui.nameEdit.text() or 'you'}!"))


app.run(Home())
