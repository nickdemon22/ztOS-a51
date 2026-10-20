"""Transport for ztMETRO's compositor-owned password sheet."""
import os
import socket
import time

class Prompt:
    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(180)
        self.sock.connect(os.path.join(os.environ['XDG_RUNTIME_DIR'], 'ztos-auth.sock'))
        self.reader = self.sock.makefile('rb')

    def send(self, kind, message=''):
        message = message.replace('\n', ' ').replace('\t', ' ')[:240]
        self.sock.sendall((kind + '\t' + message + '\n').encode())

    def password(self, message):
        self.send('PROMPT', message)
        line = self.reader.readline(1024)
        if not line.startswith(b'PASSWORD\t'):
            raise InterruptedError('Authentication cancelled or another prompt is active.')
        return bytearray(line[9:].rstrip(b'\n'))

    def retry(self, message='Incorrect password. Try again.'):
        self.send('FAIL', message)
        line = self.reader.readline(1024)
        if not line.startswith(b'PASSWORD\t'):
            raise InterruptedError('Authentication cancelled.')
        return bytearray(line[9:].rstrip(b'\n'))

    def close(self, success=False):
        if success:
            self.send('OK', 'Password accepted')
            time.sleep(.65)
        self.reader.close()
        self.sock.close()
