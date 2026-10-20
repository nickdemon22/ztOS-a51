#define main ztmetro_main
#include "../ztmetro.c"
#undef main
#include <assert.h>
int main(void) {
    struct zt_server s = { .width=360, .height=740, .scale=1, .key_height=56,
        .osk_pressed=-1, .auth_fd=-1 };
    s.wl_display=wl_display_create(); s.loop=wl_display_get_event_loop(s.wl_display);
    int peers[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,peers)==0); s.auth_fd=peers[0];
    char prompt[]="PROMPT\tTest authentication"; auth_line(&s,prompt);
    assert(s.auth_active && s.dialog_open && s.osk_visible && !s.auth_busy);
    dialog_close(&s); assert(s.dialog_open);
    auth_input(&s,KEY_A,"a"); auth_input(&s,KEY_B,"B"); auth_input(&s,KEY_3,"#");
    auth_input(&s,KEY_E,"é"); assert(s.auth_len==5);
    auth_input(&s,KEY_BACKSPACE,""); assert(s.auth_len==3);
    auth_submit(&s);
    char reply[100]={0}; assert(read(peers[1],reply,sizeof(reply))>0);
    assert(!strcmp(reply,"PASSWORD\taB#\n")); assert(s.auth_busy && !s.auth_len && !s.auth_password[0]);
    auth_input(&s,KEY_C,"c"); assert(!s.auth_len);
    char fail[]="FAIL\tIncorrect password. Try again."; auth_line(&s,fail);
    assert(!s.auth_busy && !s.auth_success);
    auth_input(&s,KEY_A,"a"); assert(s.auth_len==1);
    char ok[]="OK\tPassword accepted"; auth_line(&s,ok);
    assert(s.auth_busy && s.auth_success);
    auth_input(&s,KEY_ESC,"");
    assert(!s.auth_active && !s.dialog_open && !s.auth_len && !s.auth_password[0]);
    close(peers[1]);
    puts("PASS: modal prompt, masked UTF-8 input and deletion, submit clears secret, busy input suppressed, failure retry, success, cancel");
    return 0;
}
