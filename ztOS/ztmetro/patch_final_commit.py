with open("ztmetro.c", "r") as f:
    code = f.read()

target = """\twlr_output_state_set_buffer(&state, buffer);
\twlr_buffer_unlock(buffer);"""

replacement = """\tif (!output->enabled) {
\t\twlr_output_state_set_enabled(&state, true);
\t}
\twlr_output_state_set_buffer(&state, buffer);
\twlr_buffer_unlock(buffer);"""

if target in code:
    code = code.replace(target, replacement, 1)
    with open("ztmetro.c", "w") as f:
        f.write(code)
    print("SUCCESS: atomic enabled + buffer commit installed")
else:
    print("ERROR: target not found")
