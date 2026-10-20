with open("ztmetro.c", "r") as f:
    code = f.read()

double_ibuf = """\tstruct ion_buffer *ibuf = wl_container_of(buffer, ibuf, base);
\tibuf->allow_dmabuf = true;
\tstruct ion_buffer *ibuf = wl_container_of(buffer, ibuf, base);
\tibuf->allow_dmabuf = true;
\tGLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);
\tibuf->allow_dmabuf = false;
\tibuf->allow_dmabuf = false;"""

single_ibuf = """\tstruct ion_buffer *ibuf = wl_container_of(buffer, ibuf, base);
\tibuf->allow_dmabuf = true;
\tGLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);
\tibuf->allow_dmabuf = false;"""

if double_ibuf in code:
    code = code.replace(double_ibuf, single_ibuf, 1)

code = code.replace("\tion_buffer_sync(buffer);\n\tion_buffer_sync(buffer);", "\tion_buffer_sync(buffer);")

old_screen_set = """static void screen_set(struct zt_server *server, bool on) {
\tif (!server->output || server->screen_on == on) {
\t\treturn;
\t}
\tif (!on && server->auth_active) auth_finish(server);
\tif (on) {
\t\tperf_tell("screen on\\n");\t/* clocks back up before the first frame */
\t}
\tstruct wlr_output_state state;
\twlr_output_state_init(&state);
\twlr_output_state_set_enabled(&state, on);
\tif (!wlr_output_commit_state(server->output->wlr_output, &state)) {
\t\twlr_log(WLR_ERROR, "screen: could not switch the display %s", on ? "on" : "off");
\t\twlr_output_state_finish(&state);
\t\treturn;
\t}
\twlr_output_state_finish(&state);
\tserver->screen_on = on;
\tif (on)
\t\tbacklight_set(server->shade_brightness);"""

new_screen_set = """static void screen_set(struct zt_server *server, bool on) {
\tif (!server->output || server->screen_on == on) {
\t\treturn;
\t}
\tif (!on && server->auth_active) auth_finish(server);
\tif (on) {
\t\tperf_tell("screen on\\n");
\t\tserver->screen_on = true;
\t\tdamage(server);
\t} else {
\t\tstruct wlr_output_state state;
\t\twlr_output_state_init(&state);
\t\twlr_output_state_set_enabled(&state, false);
\t\twlr_output_commit_state(server->output->wlr_output, &state);
\t\twlr_output_state_finish(&state);
\t\tserver->screen_on = false;
\t}"""

if old_screen_set in code:
    code = code.replace(old_screen_set, new_screen_set, 1)

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: ztmetro.c patched")
