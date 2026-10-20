with open("ztmetro.c", "r") as f:
    code = f.read()

old_dmabuf_func = """static bool ion_buffer_get_dmabuf(struct wlr_buffer *wlr_buf, struct wlr_dmabuf_attributes *attribs) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);"""

new_dmabuf_func = """static bool ion_buffer_get_dmabuf(struct wlr_buffer *wlr_buf, struct wlr_dmabuf_attributes *attribs) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    if (!buf->allow_dmabuf) {
        return false;
    }"""

code = code.replace(old_dmabuf_func, new_dmabuf_func, 1)

old_struct = """struct ion_buffer {
    struct wlr_buffer base;
    int ion_fd;
    int dmabuf_fd;
    int memfd;"""

new_struct = """struct ion_buffer {
    struct wlr_buffer base;
    bool allow_dmabuf;
    int ion_fd;
    int dmabuf_fd;
    int memfd;"""

code = code.replace(old_struct, new_struct, 1)

old_alloc = """    buf->ion_fd = alloc->ion_fd;
    buf->dmabuf_fd = m.fd;"""

new_alloc = """    buf->allow_dmabuf = true;
    buf->ion_fd = alloc->ion_fd;
    buf->dmabuf_fd = m.fd;"""

code = code.replace(old_alloc, new_alloc, 1)

old_stride = """    uint32_t stride = (width * 4 + 255) & ~255;"""
new_stride = """    uint32_t stride = width * 4;"""
code = code.replace(old_stride, new_stride, 1)

old_fbo = """\tGLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);"""
new_fbo = """\tstruct ion_buffer *ibuf = wl_container_of(buffer, ibuf, base);
\tibuf->allow_dmabuf = true;
\tGLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);
\tibuf->allow_dmabuf = false;"""
code = code.replace(old_fbo, new_fbo, 1)

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: shm toggle installed")
