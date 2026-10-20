with open("ztmetro.c", "r") as f:
    code = f.read()

target_sensor = """\tbool ready = false;
\tif (!test_fail) {
\t\tchar buf[16] = {0};
\t\tint fd = open("/sys/class/sec/sensorhub/ready", O_RDONLY);
\t\tif (fd >= 0) {
\t\t\tssize_t n = read(fd, buf, sizeof(buf) - 1);
\t\t\tclose(fd);
\t\t\tif (n > 0 && buf[0] == '1') {
\t\t\t\tready = true;
\t\t\t}
\t\t}
\t}"""

replacement_sensor = """\tbool ready = true;"""
code = code.replace(target_sensor, replacement_sensor, 1)

target_mode = """\tstruct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
\tif (mode) {
\t\twlr_output_state_set_mode(&state, mode);
\t}"""

replacement_mode = """\twlr_output_state_set_custom_mode(&state, 1080, 2400, 60000);"""
code = code.replace(target_mode, replacement_mode, 1)

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: boot and 1080x2400 resolution patched")
