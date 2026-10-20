with open("ztmetro.c", "r") as f:
    code = f.read()

target_impl = """static const struct wlr_buffer_impl ion_buf_impl = {
    .destroy = ion_buffer_destroy,
    .get_dmabuf = ion_buffer_get_dmabuf,
    .begin_data_ptr_access = ion_buffer_begin_data_ptr_access,
    .end_data_ptr_access = ion_buffer_end_data_ptr_access,
};"""

replacement_impl = """static bool ion_buffer_get_shm(struct wlr_buffer *wlr_buf, struct wlr_shm_attributes *attribs) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    memset(attribs, 0, sizeof(*attribs));
    attribs->fd = fcntl(buf->dmabuf_fd, F_DUPFD_CLOEXEC, 0);
    if (attribs->fd < 0) {
        attribs->fd = dup(buf->dmabuf_fd);
    }
    attribs->format = buf->format;
    attribs->width = buf->base.width;
    attribs->height = buf->base.height;
    attribs->stride = buf->stride;
    attribs->offset = 0;
    return true;
}

static const struct wlr_buffer_impl ion_buf_impl = {
    .destroy = ion_buffer_destroy,
    .get_dmabuf = ion_buffer_get_dmabuf,
    .get_shm = ion_buffer_get_shm,
    .begin_data_ptr_access = ion_buffer_begin_data_ptr_access,
    .end_data_ptr_access = ion_buffer_end_data_ptr_access,
};"""

code = code.replace(target_impl, replacement_impl, 1)

target_caps = "WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_DATA_PTR"
replacement_caps = "WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM | WLR_BUFFER_CAP_DATA_PTR"
code = code.replace(target_caps, replacement_caps, 1)

target_init = "\twlr_output_init_render(wlr_output, server->allocator, server->renderer);"
replacement_init = """\tif (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
\t\twlr_output->allocator = server->allocator;
\t\twlr_output->renderer = server->renderer;
\t}"""
code = code.replace(target_init, replacement_init, 1)

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: shm support patched")
