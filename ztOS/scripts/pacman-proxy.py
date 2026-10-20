#!/usr/bin/env python3
"""A plain-HTTP forward proxy for the phone, so pacman can reach a mirror.

  scripts/pacman-proxy.py [bind-address] [port]

The phone's only link is the USB network to this host, which does not route
anywhere. Rather than touch the host's firewall (NAT needs root), pacman is
pointed at this proxy with curl's -x: it is GET-only, allows nothing but the
Arch Linux ARM mirrors, and logs every request.
"""
import http.server
import re
import socketserver
import sys
import urllib.error
import urllib.request

BIND = sys.argv[1] if len(sys.argv) > 1 else "172.16.42.2"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
ALLOWED = re.compile(
    r"^http://([a-z0-9.-]+\.)?(archlinuxarm\.org|archlinux\.org|kernel\.org)(/|$)")


class Proxy(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.client_address[0], fmt % a))

    def do_GET(self, body=True):
        if not ALLOWED.match(self.path):
            self.send_error(403, "not an Arch Linux ARM mirror")
            return
        req = urllib.request.Request(self.path, headers={"User-Agent": "zboot-proxy"})
        try:
            # Streamed, not buffered: pacman gives up on a transfer that moves
            # no bytes for ten seconds, and a 20 MB database read in full
            # before the first byte is sent looks exactly like a stall.
            with urllib.request.urlopen(req, timeout=120) as up:
                length = up.headers.get("Content-Length")
                self.send_response(up.status)
                self.send_header("Content-Type", up.headers.get("Content-Type", "application/octet-stream"))
                if length is not None:
                    self.send_header("Content-Length", length)
                else:
                    self.send_header("Connection", "close")
                self.end_headers()
                if not body:
                    return
                while True:
                    chunk = up.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        except urllib.error.HTTPError as e:
            self.send_error(e.code, e.reason)
        except Exception as e:  # noqa: BLE001 - the phone only needs a status
            self.send_error(502, str(e))

    def do_HEAD(self):
        self.do_GET(body=False)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    # pacman opens several connections at once; the default backlog of 5 makes
    # the extra ones wait in the kernel long enough for its stall detector.
    request_queue_size = 64


print(f"proxy on {BIND}:{PORT}, Arch Linux ARM mirrors only", file=sys.stderr)
Server((BIND, PORT), Proxy).serve_forever()
