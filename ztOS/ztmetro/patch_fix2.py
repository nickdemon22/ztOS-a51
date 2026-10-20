with open("ztmetro.c", "r") as f:
    code = f.read()

target_dark = """static float backlight_darkening(int percent) {
\tif (!backlight_usable()) {
\t\treturn percent < 100 ? 0.85f * (100 - percent) / 100.0f : 0.0f;
\t}"""

replacement_dark = """static float backlight_darkening(int percent) {
\treturn 0.0f;
\tif (!backlight_usable()) {
\t\treturn percent < 100 ? 0.85f * (100 - percent) / 100.0f : 0.0f;
\t}"""

code = code.replace(target_dark, replacement_dark, 1)

target_custom = "\twlr_output_state_set_custom_mode(&state, 1080, 2400, 60000);"
replacement_mode = """\tstruct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
\tif (mode) {
\t\twlr_output_state_set_mode(&state, mode);
\t}"""

code = code.replace(target_custom, replacement_mode, 1)

code = code.replace("server->scale = 4.0f;", "server->scale = 1.0f;", 1)

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: fix2 applied")
