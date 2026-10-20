with open("ztmetro.c", "r") as f:
    code = f.read()

target = """\tstruct wlr_output_state state;
\twlr_output_state_init(&state);
\twlr_output_state_set_enabled(&state, true);
\tstruct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
\tif (mode) {
\t\twlr_output_state_set_mode(&state, mode);
\t}
\twlr_output_state_set_scale(&state, server->scale);"""

replacement = """\tstruct wlr_output_state state;
\twlr_output_state_init(&state);
\twlr_output_state_set_enabled(&state, true);
\twlr_output_state_set_scale(&state, server->scale);"""

if target in code:
    code = code.replace(target, replacement, 1)
    with open("ztmetro.c", "w") as f:
        f.write(code)
    print("SUCCESS: removed set_mode from initial commit")
else:
    print("ERROR: target not found")
