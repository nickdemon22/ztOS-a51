"""Metro text messaging frontend; Chromium stays behind a private stdio bridge."""
import base64
import json
import os
from pathlib import Path
import ztos
from PySide6.QtCore import QProcess, QTimer, Qt
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QLabel, QWidget, QVBoxLayout

APP = ztos.App()


class Bridge:
    def __init__(self):
        self.process = QProcess(APP.window)
        self.process.setProcessChannelMode(QProcess.SeparateChannels)
        self.buffer = bytearray()
        self.pending = {}
        self.timeouts = {}
        self.sequence = 0
        self.listener = lambda event: None
        self.process.readyReadStandardOutput.connect(self.read)
        self.process.readyReadStandardError.connect(lambda: self.process.readAllStandardError())
        self.process.errorOccurred.connect(lambda _: self.listener({'event':'error','error':'Could not start Node.js. Check app dependencies.'}))
        self.process.finished.connect(self.finished)
        APP.qt.aboutToQuit.connect(self.stop)
        self.installer = QProcess(APP.window)
        self.installer.setProcessChannelMode(QProcess.MergedChannels)
        self.installer.readyReadStandardOutput.connect(lambda: self.installer.readAllStandardOutput())
        self.installer.finished.connect(self.installed)
        self.installer.errorOccurred.connect(lambda _: self.listener({'event':'error','error':'Could not start npm. Install the npm system package.'}))

    def start(self):
        root = Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'ztos/whatsapp'
        root.mkdir(parents=True, exist_ok=True, mode=0o700)
        for name in ('bridge.js', 'package.json', 'package-lock.json'):
            (root/name).write_bytes(ztos.resource(name))
        if not (root/'node_modules/whatsapp-web.js').is_dir():
            self.listener({'event':'status','text':'Setting up WhatsApp for the first time… Internet is required.'})
            from PySide6.QtCore import QProcessEnvironment
            env = QProcessEnvironment.systemEnvironment()
            env.insert('PUPPETEER_SKIP_DOWNLOAD', 'true')
            self.installer.setProcessEnvironment(env)
            self.installer.setWorkingDirectory(str(root))
            self.installer.start('npm', ['ci', '--omit=dev', '--no-audit', '--no-fund'])
            return
        from PySide6.QtCore import QProcessEnvironment
        env = QProcessEnvironment.systemEnvironment()
        env.insert('ZTOS_WHATSAPP_DATA', str(root))
        self.process.setProcessEnvironment(env)
        self.process.setWorkingDirectory(str(root))
        self.process.start('node', [str(root/'bridge.js')])

    def installed(self, code, status):
        if code == 0 and status == QProcess.NormalExit:
            self.start()
        else:
            self.listener({'event':'error','error':'Setup failed. Check your internet connection and reopen WhatsApp to retry.'})

    def request(self, method, callback, **fields):
        if self.process.state() != QProcess.Running:
            callback(None, 'WhatsApp is not connected')
            return
        self.sequence += 1
        ident = self.sequence
        self.pending[ident] = callback
        timer = QTimer(APP.window, singleShot=True, interval=60000)
        timer.timeout.connect(lambda: self.expired(ident))
        self.timeouts[ident] = timer
        timer.start()
        self.process.write((json.dumps(dict(id=ident, method=method, **fields))+'\n').encode())

    def expired(self, ident):
        timer = self.timeouts.pop(ident, None)
        if timer:
            timer.deleteLater()
        callback = self.pending.pop(ident, None)
        if callback:
            callback(None, 'Request timed out. Check the conversation before retrying a send.')

    def finished(self, *_):
        for ident in list(self.pending):
            self.expired(ident)
        self.listener({'event':'error','error':'WhatsApp stopped. Reopen the app to reconnect.'})

    def read(self):
        self.buffer.extend(bytes(self.process.readAllStandardOutput()))
        while b'\n' in self.buffer:
            line, _, self.buffer = self.buffer.partition(b'\n')
            try:
                data = json.loads(line)
            except (ValueError, UnicodeError):
                continue
            callback = self.pending.pop(data.get('id'), None)
            if callback:
                timer = self.timeouts.pop(data['id'], None)
                if timer:
                    timer.stop()
                    timer.deleteLater()
                callback(data.get('result'), data.get('error'))
            else:
                self.listener(data)

    def stop(self):
        if self.installer.state() != QProcess.NotRunning:
            self.installer.kill()
            self.installer.waitForFinished(1000)
        self.process.closeWriteChannel()
        if not self.process.waitForFinished(1500):
            self.process.terminate()
            if not self.process.waitForFinished(1000):
                self.process.kill()
                self.process.waitForFinished(1000)


class Rows(QWidget):
    def __init__(self):
        super().__init__()
        self.rows = QVBoxLayout(self)
        self.rows.setContentsMargins(0,0,0,0)
        self.rows.setSpacing(0)

    def clear(self):
        while self.rows.count():
            item = self.rows.takeAt(0)
            if item.widget():
                item.widget().deleteLater()


class Conversation(ztos.Page):
    def __init__(self, bridge, chat):
        super().__init__(chat['name'])
        self.bridge, self.chat = bridge, chat
        self.ids = set()
        self.status = self.text('Loading messages…', size=13, color=ztos.GREY)
        self.messages = self.add(Rows())
        self.composer = ztos.TextBox('', placeholder='Message', on_submit=lambda _: self.send())
        # Keep the composer outside the scrolling history.
        self.layout().addWidget(self.composer)
        self.send_button = ztos.RoundButton('\uE724', 'send', self.send)
        self.bar(self.send_button)
        self.bridge.request('messages', self.loaded, chat=chat['id'])

    def loaded(self, messages, error):
        if error:
            self.status.setText(error)
            return
        self.status.setText('Text messages')
        self.messages.clear()
        self.ids.clear()
        for msg in messages:
            self.append(msg, scroll=False)
        QTimer.singleShot(0, lambda: self.scroll_to(self._max()))

    def append(self, msg, scroll=True):
        if msg['id'] in self.ids:
            return
        self.ids.add(msg['id'])
        label = QLabel(('you\n' if msg['fromMe'] else '') + msg['body'])
        label.setTextFormat(Qt.PlainText)
        label.setWordWrap(True)
        label.setAlignment(Qt.AlignRight if msg['fromMe'] else Qt.AlignLeft)
        label.setContentsMargins(0,10,0,10)
        self.messages.rows.addWidget(label)
        if scroll:
            QTimer.singleShot(0, lambda: self.scroll_to(self._max()))

    def send(self):
        text = self.composer.text().strip()
        if not text or not self.send_button.isEnabled():
            return
        self.send_button.setEnabled(False)
        self.status.setText('Sending…')
        def done(msg, error):
            self.send_button.setEnabled(True)
            if error:
                self.status.setText('Not sent · '+error)
                return
            if self.composer.text().strip() == text:
                self.composer.field.clear()
            self.status.setText('Sent')
            self.append(msg)
        self.bridge.request('send', done, chat=self.chat['id'], text=text)


class Chats(ztos.Page):
    def __init__(self):
        super().__init__('chats')
        self.bridge = Bridge()
        self.bridge.listener = self.backend_event
        self.refresh_timer = QTimer(self, singleShot=True, interval=800)
        self.refresh_timer.timeout.connect(self.refresh)
        self.status = self.text('Connecting to WhatsApp…', size=15)
        self.qr = self.add(QLabel())
        self.qr.setAlignment(Qt.AlignCenter)
        self.help = self.text('On your phone: WhatsApp → Linked devices → Link a device. Scan the code here.', size=14, color=ztos.GREY)
        self.chats = self.add(Rows())
        self.bar(ztos.RoundButton('\uE72C','refresh',self.refresh))
        QTimer.singleShot(0, self.bridge.start)

    def backend_event(self, event):
        kind = event.get('event')
        if kind == 'qr':
            image = QPixmap()
            image.loadFromData(base64.b64decode(event['image'].split(',',1)[1]))
            self.qr.setPixmap(image)
            self.qr.show()
            self.help.show()
            self.status.setText('Link WhatsApp')
        elif kind == 'ready':
            self.qr.clear()
            self.qr.hide()
            self.help.hide()
            self.status.setText('Connected')
            self.refresh()
        elif kind in ('error','status'):
            self.status.setText(event.get('error') or event.get('text'))
        elif kind == 'message':
            page = APP.page
            if isinstance(page, Conversation) and page.chat['id'] == event['chat']:
                page.append(event['message'])
            self.refresh_timer.start()

    def refresh(self):
        self.bridge.request('chats', self.loaded)

    def loaded(self, chats, error):
        if error:
            self.status.setText(error)
            return
        self.chats.clear()
        self.status.setText(f'{len(chats)} chats')
        for chat in chats:
            unread = f" · {chat['unread']} unread" if chat['unread'] else ''
            entry = ztos.Entry(chat['name']+unread, (chat['last'] or 'Open conversation')[:100],
                               on_tap=lambda c=chat: APP.push(Conversation(self.bridge,c)))
            self.chats.rows.addWidget(entry)

APP.run(Chats())
