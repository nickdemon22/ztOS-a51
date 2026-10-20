# WhatsApp for ztOS

A Metro frontend with a private whatsapp-web.js subprocess. Runs in `eco` mode.
No TCP server, message polling, notifications, media downloads, or background service.
Closing the application closes its browser; the linked session stays on disk.

First launch installs the locked Node dependencies into
`~/.local/share/ztos/whatsapp`, using the system Chromium rather than downloading
another browser. Requires internet, `nodejs`, `npm`, and `chromium`. The installer
includes those packages. Install onto an existing phone with:

```
sdk/bin/ztos-sdk install -C os/apps/whatsapp --device zerone@172.16.42.1
```

Link by scanning the displayed QR code from WhatsApp → Linked devices → Link a
device. Open an existing chat, read its latest 50 messages, and send text. The
app keeps drafts when sending fails and deduplicates outgoing message echoes.
Refresh reloads up to 100 recent chats. Linking requires a second screen/device
that can scan the displayed code. Session files are private to the Linux user.

This is an unofficial WhatsApp client. Chromium still runs WhatsApp Web;
"minimal" describes the bridge and UI, not a replacement for the web engine.

Implementation references:
- https://wwebjs.dev/guide/creating-your-bot/authentication.html
- https://docs.wwebjs.dev/Client.html
- https://docs.wwebjs.dev/Chat.html
