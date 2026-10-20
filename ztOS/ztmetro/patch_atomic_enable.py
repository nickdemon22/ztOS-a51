with open("ztmetro.c", "r") as f:
    code = f.read()

target_server_output = """\tstruct wlr_output_state state;
\twlr_output_state_init(&state);
\twlr_output_state_set_enabled(&state, true);
\twlr_output_state_set_scale(&state, server->scale);
\tif (!wlr_output_commit_state(wlr_output, &state)) {
\t\twlr_log(WLR_ERROR, "server_new_output: INITIAL COMMIT FAILED!");
\t} else {
\t\twlr_log(WLR_INFO, "server_new_output: INITIAL COMMIT OK, enabled=%d", wlr_output->enabled);
\t}
\twlr_output_state_finish(&state);"""

replacement_server_output = """\tserver->width = wlr_output->width;
\tserver->height = wlr_output->height;"""
code = code.replace(target_server_output, replacement_server_output, 1)

target_frame_commit = """\twlr_output_state_set_buffer(&state, buffer);
\twlr_buffer_unlock(buffer);"""

replacement_frame_commit = """\tif (!output->enabled) {
\t\twlr_output_state_set_enabled(&state, true);
\t}
\twlr_output_state_set_buffer(&state, buffer);
\twlr_buffer_unlock(buffer);"""
code = code.replace(target_frame_commit, replacement_frame_commit, 1)

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: atomic enable patched")
