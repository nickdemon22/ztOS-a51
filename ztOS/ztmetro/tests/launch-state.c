#define main ztmetro_main
#include "../ztmetro.c"
#undef main
#include <assert.h>

int main(void) {
    struct zt_server s = {0};
    s.wl_display = wl_display_create();
    s.loop = wl_display_get_event_loop(s.wl_display);
    s.launch_timer = wl_event_loop_add_timer(s.loop, launch_timed_out, &s);
    s.loading_since = now_ms();
    assert(loading_active(&s));
    s.locked = true; assert(!loading_active(&s)); s.locked = false;
    s.switch_t = 1; assert(!loading_active(&s)); s.switch_t = 0;
    struct zt_toplevel t = {0}; s.shown = &t;
    assert(!loading_active(&s));
    t.awaiting = true; assert(loading_active(&s));
    s.shown = NULL;
    s.pending_pid = getpid();
    snprintf(s.pending_command, sizeof(s.pending_command), "slow-app");
    launch_timed_out(&s);
    assert(s.loading_since && s.pending_pid == getpid() && s.pending_command[0]);
    s.pending_pid = 0;
    launch_timed_out(&s);
    assert(!s.loading_since && !s.pending_command[0]);
    assert(LAUNCH_WAIT_MS >= 30000);
    puts("PASS: loading frame guard, mapped/awaiting window states, slow live process stays loading, failed launch clears state, cold-start grace");
    return 0;
}
