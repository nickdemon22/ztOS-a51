with open("ztmetro.c", "r") as f:
    code = f.read()

target = """\tstruct wlr_output_state state;
\twlr_output_state_init(&state);
\tif (!wlr_output_configure_primary_swapchain(output, &state, &output->swapchain)) {"""

replacement = """\tstruct wlr_output_state state;
\twlr_output_state_init(&state);
\twlr_output_state_set_enabled(&state, true);
\tif (!wlr_output_configure_primary_swapchain(output, &state, &output->swapchain)) {"""

if target in code:
    code = code.replace(target, replacement, 1)
    with open("ztmetro.c", "w") as f:
        f.write(code)
    print("SUCCESS: swapchain enable patched")
else:
    print("ERROR: target not found")
