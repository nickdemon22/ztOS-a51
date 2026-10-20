with open("ztmetro.c", "r") as f:
    code = f.read()

start_marker = "struct ion_buffer {"
end_marker = "static struct wlr_allocator *ion_allocator_create(void) {"

idx1 = code.find(start_marker)
idx2 = code.find(end_marker)

if idx1 == -1 or idx2 == -1:
    print("ERROR: markers not found", idx1, idx2)
    exit(1)

new_block = """int memfd_create(const char *name, unsigned int flags);

struct ion_buffer {
    struct wlr_buffer base;
    int ion_fd;
    int dmabuf_fd;
    int memfd;
    void *ion_data;
    void *memfd_data;
    size_t size;
    uint32_t format;
    uint32_t stride;
};

static void ion_buffer_destroy(struct wlr_buffer *wlr_buf) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    if (buf->ion_data && buf->ion_data != MAP_FAILED) {
        munmap(buf->ion_data, buf->size);
    }
    if (buf->memfd_data && buf->memfd_data != MAP_FAILED) {
        munmap(buf->memfd_data, buf->size);
    }
    if (buf->dmabuf_fd >= 0) close(buf->dmabuf_fd);
    if (buf->memfd >= 0) close(buf->memfd);
    wlr_buffer_finish(&buf->base);
    free(buf);
}

static bool ion_buffer_get_dmabuf(struct wlr_buffer *wlr_buf, struct wlr_dmabuf_attributes *attribs) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    memset(attribs, 0, sizeof(*attribs));
    attribs->width = buf->base.width;
    attribs->height = buf->base.height;
    attribs->format = buf->format;
    attribs->modifier = DRM_FORMAT_MOD_LINEAR;
    attribs->n_planes = 1;
    attribs->offset[0] = 0;
    attribs->stride[0] = buf->stride;
    attribs->fd[0] = fcntl(buf->dmabuf_fd, F_DUPFD_CLOEXEC, 0);
    if (attribs->fd[0] < 0) {
        attribs->fd = dup(buf->dmabuf_fd);
    }
    return true;
}

static bool ion_buffer_get_shm(struct wlr_buffer *wlr_buf, struct wlr_shm_attributes *attribs) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    memset(attribs, 0, sizeof(*attribs));
    attribs->fd = fcntl(buf->memfd, F_DUPFD_CLOEXEC, 0);
    if (attribs->fd < 0) {
        attribs->fd = dup(buf->memfd);
    }
    attribs->format = buf->format;
    attribs->width = buf->base.width;
    attribs->height = buf->base.height;
    attribs->stride = buf->stride;
    attribs->offset = 0;
    return true;
}

static bool ion_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buf, uint32_t flags,
        void **data, uint32_t *format, size_t *stride) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    *data = buf->memfd_data;
    *format = buf->format;
    *stride = buf->stride;
    return true;
}

static void ion_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buf) {
}

static void ion_buffer_sync(struct wlr_buffer *wlr_buf) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    if (buf->ion_data && buf->memfd_data) {
        memcpy(buf->memfd_data, buf->ion_data, buf->size);
    }
}

static const struct wlr_buffer_impl ion_buf_impl = {
    .destroy = ion_buffer_destroy,
    .get_dmabuf = ion_buffer_get_dmabuf,
    .get_shm = ion_buffer_get_shm,
    .begin_data_ptr_access = ion_buffer_begin_data_ptr_access,
    .end_data_ptr_access = ion_buffer_end_data_ptr_access,
};

struct ion_allocator {
    struct wlr_allocator base;
    int ion_fd;
};

static struct wlr_buffer *ion_allocator_create_buffer(struct wlr_allocator *wlr_alloc,
        int width, int height, const struct wlr_drm_format *format) {
    struct ion_allocator *alloc = wl_container_of(wlr_alloc, alloc, base);
    uint32_t drm_fmt = format ? format->format : DRM_FORMAT_ARGB8888;
    uint32_t stride = (width * 4 + 255) & ~255;
    size_t size = ((size_t)stride * height + 4095) & ~4095;

    struct ion_allocation_data_modern m = {0};
    m.len = size;
    m.heap_id_mask = 0xffffffff;
    if (ioctl(alloc->ion_fd, ION_IOC_ALLOC_MODERN, &m) < 0) {
        wlr_log(WLR_ERROR, "ion ioctl failed: %s", strerror(errno));
        return NULL;
    }

    int memfd = memfd_create("wlr_shm", 1);
    if (memfd < 0) {
        close(m.fd);
        return NULL;
    }
    if (ftruncate(memfd, size) < 0) {
        close(memfd);
        close(m.fd);
        return NULL;
    }

    void *ion_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 0);
    void *memfd_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);

    struct ion_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        close(memfd);
        close(m.fd);
        return NULL;
    }
    buf->ion_fd = alloc->ion_fd;
    buf->dmabuf_fd = m.fd;
    buf->memfd = memfd;
    buf->ion_data = ion_data;
    buf->memfd_data = memfd_data;
    buf->size = size;
    buf->format = drm_fmt;
    buf->stride = stride;

    wlr_buffer_init(&buf->base, &ion_buf_impl, width, height);
    return &buf->base;
}

static void ion_allocator_destroy(struct wlr_allocator *wlr_alloc) {
    struct ion_allocator *alloc = wl_container_of(wlr_alloc, alloc, base);
    if (alloc->ion_fd >= 0) {
        close(alloc->ion_fd);
    }
    free(alloc);
}

static const struct wlr_allocator_interface ion_alloc_impl = {
    .create_buffer = ion_allocator_create_buffer,
    .destroy = ion_allocator_destroy,
};

"""

code = code[:idx1] + new_block + code[idx2:]

with open("ztmetro.c", "w") as f:
    f.write(code)

print("SUCCESS: memfd exact patch applied")
