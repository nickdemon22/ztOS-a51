with open("ztmetro.c", "r") as f:
    code = f.read()

target_render = "static void render_frame(struct zt_server *server) {"
replacement_render = """static void render_frame(struct zt_server *server) {
\twlr_log(WLR_INFO, "RENDER_FRAME EXECUTING!");"""
code = code.replace(target_render, replacement_render, 1)

target_frame = """static void output_frame(struct wl_listener *listener, void *data) {
\tstruct zt_output *output = wl_container_of(listener, output, frame);
\tstruct zt_server *server = output->server;"""
replacement_frame = """static void output_frame(struct wl_listener *listener, void *data) {
\tstruct zt_output *output = wl_container_of(listener, output, frame);
\tstruct zt_server *server = output->server;
\twlr_log(WLR_INFO, "FRAME_TICK: dirty=%d moving=%d", server->dirty, server->moving);"""
code = code.replace(target_frame, replacement_frame, 1)

target_splash = "\tserver->splash = true;\n\tdraw_splash(server);"
replacement_splash = """\tserver->splash = true;\n\tdraw_splash(server);\n\trender_frame(server);"""
code = code.replace(target_splash, replacement_splash, 1)

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: debug hooks installed")
