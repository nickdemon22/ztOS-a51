# Authentication and date settings

ztMETRO owns the top password sheet and captures keyboard input while it is
open. The focused application receives no password keystrokes. Text is masked,
key previews and prediction are disabled, and the input buffer is cleared on
submission, retry, cancellation, disconnect, or screen-off. The Unix socket
lives in the user's private runtime directory, is mode 0600, and accepts only
clients with the same UID. Requests contain a message; they cannot execute a
command in the compositor.

`ztos-auth COMMAND ARG...` uses sudo's authentication and policy, with up to
three attempts and explicit incorrect/accepted feedback. Valid sudo tickets
are reused. Secrets are passed through stdin, never command arguments or a
password file. `ztos-askpass` is also provided for `sudo -A`; bare terminal sudo
continues to use its terminal prompt. The askpass adapter supplies input;
sudo remains responsible for checking it.

The native `ztos-polkit-agent` registers for the graphical login session and
uses the same sheet for polkit requests, including pkexec. Verification and
permissions remain with polkit. The agent builds against polkit and GLib,
already provided by the base package set; it does not need Python GI bindings.
It starts with the compositor and exits when its parent exits.

Settings → date + region exposes automatic time, manual date/time, IANA time
zone, regional formats, NTP servers, and sync now. Choosing a region suggests
its country pool; the server field remains editable. Sync now enables NTP,
restarts systemd-timesyncd, and checks for a newly updated synchronization
marker. If a server does not respond within 20 seconds, the UI reports that
synchronization is still pending. Saving regional formats affects newly
opened applications and does not change their display language. Date changes
use the interactive handler; there is no passwordless date-setting rule.

The installer builds and installs the native agent with ztMETRO, then installs
the Python wrappers, backend and shared transport from the OS overlay. Fresh
builds include these components. Verify with `make check-auth check-launch
check-keyboard` on the phone and `python ztmetro/tests/date-region.py` on the
host. Real sudo and pkexec authentication, wrong-password retry, root command
execution, live compositor cancellation, and a live NTP sync were also tested.
