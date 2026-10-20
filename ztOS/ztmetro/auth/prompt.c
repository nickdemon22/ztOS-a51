/* Included by the compositor after its dialog helpers. Passwords stay in RAM. */
static void auth_finish(struct zt_server *server) {
    if (server->auth_source) wl_event_source_remove(server->auth_source);
    server->auth_source = NULL;
    if (server->auth_fd >= 0) close(server->auth_fd);
    server->auth_fd = -1;
    bool was_active = server->auth_active;
    server->auth_active = false;
    explicit_bzero(server->auth_password, sizeof(server->auth_password));
    server->auth_len = server->auth_read_len = 0;
    if (was_active) {
        dialog_close(server);
        osk_show(server, server->auth_osk_was_visible);
    }
}

static void auth_submit(struct zt_server *server) {
    if (!server->auth_active || server->auth_busy) return;
    char reply[sizeof(server->auth_password)+16];
    int n = snprintf(reply, sizeof(reply), "PASSWORD\t%s\n", server->auth_password);
    if (send(server->auth_fd, reply, n, MSG_NOSIGNAL) != n) {
        explicit_bzero(reply, sizeof(reply)); auth_finish(server); return;
    }
    explicit_bzero(reply, sizeof(reply));
    explicit_bzero(server->auth_password, sizeof(server->auth_password));
    server->auth_len = 0;
    server->auth_busy = true;
    server->auth_success = false;
    snprintf(server->auth_status, sizeof(server->auth_status), "Checking password...");
    draw_dialog(server);
    damage(server);
}

static void auth_input(struct zt_server *server, uint32_t keycode, const char *text) {
    if (keycode == KEY_ESC) {
        send(server->auth_fd, "CANCEL\n", 7, MSG_NOSIGNAL);
        auth_finish(server); return;
    }
    if (server->auth_busy) return;
    if (keycode == KEY_ENTER || keycode == KEY_KPENTER) { auth_submit(server); return; }
    if (keycode == KEY_BACKSPACE) {
        if (server->auth_len) {
            size_t pos = server->auth_len-1;
            while (pos && ((unsigned char)server->auth_password[pos] & 0xc0) == 0x80) pos--;
            explicit_bzero(server->auth_password+pos, server->auth_len-pos);
            server->auth_len = pos;
        }
    } else if (text && text[0] && (unsigned char)text[0] >= 32 && text[0] != 127) {
        size_t n = strlen(text);
        if (server->auth_len+n < sizeof(server->auth_password)) {
            memcpy(server->auth_password+server->auth_len, text, n+1);
            server->auth_len += n;
        }
    }
    draw_dialog(server);
    damage(server);
}

static void auth_line(struct zt_server *server, char *line) {
    if (!strncmp(line, "PROMPT\t", 7)) {
        if (server->locked || server->splash) {
            send(server->auth_fd, "CANCEL\n", 7, MSG_NOSIGNAL);
            auth_finish(server); return;
        }
        if (!server->auth_active) server->auth_osk_was_visible = server->osk_visible;
        server->auth_active = true;
        server->auth_busy = server->auth_success = false;
        explicit_bzero(server->auth_password, sizeof(server->auth_password));
        server->auth_len = 0;
        snprintf(server->auth_status, sizeof(server->auth_status), "Enter your password");
        dialog_show(server, "Authentication required", line+7,
            "authenticate", "auth_submit", "cancel", "auth_cancel");
        server->osk_terminal = false;
        server->key_prefix[0] = 0;
        osk_show(server, true);
    } else if (server->auth_active && !strncmp(line, "FAIL\t", 5)) {
        server->auth_busy = false;
        server->auth_success = false;
        explicit_bzero(server->auth_password, sizeof(server->auth_password));
        server->auth_len = 0;
        snprintf(server->auth_status, sizeof(server->auth_status), "%s", line+5);
        draw_dialog(server); damage(server);
    } else if (server->auth_active && !strncmp(line, "OK\t", 3)) {
        server->auth_busy = server->auth_success = true;
        explicit_bzero(server->auth_password, sizeof(server->auth_password));
        server->auth_len = 0;
        snprintf(server->auth_status, sizeof(server->auth_status), "%s", line+3);
        draw_dialog(server); damage(server);
    }
}

static int auth_readable(int fd, uint32_t mask, void *data) {
    struct zt_server *server = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { auth_finish(server); return 0; }
    ssize_t n = read(fd, server->auth_read+server->auth_read_len,
        sizeof(server->auth_read)-1-server->auth_read_len);
    if (n <= 0) {
        if (!n || (errno != EAGAIN && errno != EINTR)) auth_finish(server);
        return 0;
    }
    server->auth_read_len += n;
    server->auth_read[server->auth_read_len] = 0;
    char *end;
    while ((end = strchr(server->auth_read, '\n'))) {
        *end = 0;
        auth_line(server, server->auth_read);
        if (server->auth_fd < 0) return 0;
        size_t rest = server->auth_read_len-(end-server->auth_read)-1;
        memmove(server->auth_read, end+1, rest);
        server->auth_read_len = rest;
        server->auth_read[rest] = 0;
    }
    if (server->auth_read_len == sizeof(server->auth_read)-1) auth_finish(server);
    return 0;
}

static int auth_accept(int fd, uint32_t mask, void *data) {
    struct zt_server *server = data;
    int peer = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (peer < 0) return 0;
    struct ucred cred; socklen_t size = sizeof(cred);
    if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &cred, &size) || cred.uid != getuid()) {
        close(peer); return 0;
    }
    if (server->auth_source) {
        send(peer, "BUSY\n", 5, MSG_NOSIGNAL); close(peer); return 0;
    }
    server->auth_fd = peer;
    server->auth_read_len = 0;
    server->auth_source = wl_event_loop_add_fd(server->loop, peer,
        WL_EVENT_READABLE, auth_readable, server);
    return 0;
}

static void auth_init(struct zt_server *server) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) return;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/ztos-auth.sock", runtime) >= sizeof(addr.sun_path)) return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    unlink(addr.sun_path);
    mode_t old = umask(0077);
    int result = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old);
    if (result || listen(fd, 4)) { close(fd); return; }
    wl_event_loop_add_fd(server->loop, fd, WL_EVENT_READABLE, auth_accept, server);
}
