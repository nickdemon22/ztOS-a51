with open("ztmetro.c", "r") as f:
    code = f.read()

target_new = "\twlr_output_commit_state(wlr_output, &state);"
replacement_new = """\tif (!wlr_output_commit_state(wlr_output, &state)) {
\t\twlr_log(WLR_ERROR, "server_new_output: INITIAL COMMIT FAILED!");
\t} else {
\t\twlr_log(WLR_INFO, "server_new_output: INITIAL COMMIT OK, enabled=%d", wlr_output->enabled);
\t}"""
code = code.replace(target_new, replacement_new, 1)

target_commit = "\twlr_output_state_set_buffer(&state, buffer);\n\twlr_buffer_unlock(buffer);"
replacement_commit = """\twlr_log(WLR_INFO, "COMMIT_TEST: out_enabled=%d out=%dx%d buf=%dx%d", output->enabled, output->width, output->height, buffer->width, buffer->height);
\twlr_output_state_set_buffer(&state, buffer);
\twlr_buffer_unlock(buffer);"""
code = code.replace(target_commit, replacement_commit, 1)

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: commit checks patched")
