with open("ztmetro.c", "r") as f:
    code = f.read()

includes = """#include <wlr/render/allocator.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/dmabuf.h>
#include <drm_fourcc.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241
#endif

struct ion_allocation_data_modern {
    uint64_t len;
    uint32_t heap_id_mask;
    uint32_t flags;
    uint32_t fd;
    uint32_t unused;
};

#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC_MODERN _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data_modern)

struct ion_buffer {
    struct wlr_buffer base;
    int ion_fd;
    int dmabuf_fd;
    size_t size;
    void *data;
    uint32_t format;
    uint32_t stride;
};

static void ion_buffer_destroy(struct wlr_buffer *wlr_buf) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    if (buf->data && buf->data != MAP_FAILED) {
        munmap(buf->data, buf->size);
    }
    if (buf->dmabuf_fd >= 0) {
        close(buf->dmabuf_fd);
    }
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
        attribs->fd[0] = dup(buf->dmabuf_fd);
    }
    return true;
}

static bool ion_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buf, uint32_t flags,
        void **data, uint32_t *format, size_t *stride) {
    struct ion_buffer *buf = wl_container_of(wlr_buf, buf, base);
    if (!buf->data) {
        buf->data = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, buf->dmabuf_fd, 0);
    }
    if (buf->data == MAP_FAILED) {
        return false;
    }
    *data = buf->data;
    *format = buf->format;
    *stride = buf->stride;
    return true;
}

static void ion_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buf) {
}

static const struct wlr_buffer_impl ion_buf_impl = {
    .destroy = ion_buffer_destroy,
    .get_dmabuf = ion_buffer_get_dmabuf,
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

    struct ion_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        close(m.fd);
        return NULL;
    }
    buf->ion_fd = alloc->ion_fd;
    buf->dmabuf_fd = m.fd;
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

static struct wlr_allocator *ion_allocator_create(void) {
    int fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "Failed to open /dev/ion: %s", strerror(errno));
        return NULL;
    }
    struct ion_allocator *alloc = calloc(1, sizeof(*alloc));
    if (!alloc) {
        close(fd);
        return NULL;
    }
    alloc->ion_fd = fd;
    wlr_allocator_init(&alloc->base, &ion_alloc_impl,
        WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_DATA_PTR);
    return &alloc->base;
}
"""

code = code.replace("#include <wlr/render/allocator.h>", includes, 1)

old_alloc = """\tstruct wlr_allocator *wlr_shm_allocator_create(void);
\tserver.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
\tif (!server.allocator) {
\t\tserver.allocator = wlr_shm_allocator_create();
\t\tif (server.allocator) {
\t\t\tserver.allocator->buffer_caps |= (1 << 1);
\t\t}
\t}"""

new_alloc = """\tserver.allocator = ion_allocator_create();
\tif (!server.allocator) {
\t\tserver.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
\t}"""

if old_alloc in code:
    code = code.replace(old_alloc, new_alloc, 1)
    with open("ztmetro.c", "w") as f:
        f.write(code)
    print("SUCCESS: ztmetro.c patched")
else:
    print("ERROR: old_alloc pattern not found")
