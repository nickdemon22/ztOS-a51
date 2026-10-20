#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <cairo.h>
#include <pango/pangocairo.h>

int main(int argc, char **argv) {
	const char *png_path = NULL;
	const char *raw_path = NULL;
	if (argc >= 3 && strcmp(argv[1], "--png") == 0) {
		png_path = argv[2];
	} else if (argc >= 3 && strcmp(argv[1], "--raw") == 0) {
		raw_path = argv[2];
	}

	uint32_t width = 1440;
	uint32_t height = 2960;
	int fb_fd = -1;
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	uint8_t *fbp = NULL;
	size_t screensize = 0;

	if (!png_path && !raw_path) {
		fb_fd = open("/dev/fb0", O_RDWR);
		if (fb_fd >= 0) {
			if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
			    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
				width = vinfo.xres;
				height = vinfo.yres;
				screensize = finfo.line_length * vinfo.yres;
				fbp = mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
				if (fbp == MAP_FAILED) {
					fbp = NULL;
				}
			}
		}
	}

	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create(surf);

	/* Background: black */
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	/* Plain ztOS logo: matching ztmetro draw_splash identically */
	cairo_scale(cr, 4.0, 4.0);
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string("Segoe WP Light, Selawik Light, Open Sans Light, sans-serif");
	pango_font_description_set_absolute_size(desc, 56.0 * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, "ztOS", -1);
	pango_layout_set_width(layout, (int)(360.0 * PANGO_SCALE));
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	double y = (740.0 - th) / 2.0;
	cairo_move_to(cr, 0, y);
	pango_cairo_show_layout(cr, layout);

	pango_font_description_free(desc);
	g_object_unref(layout);
	cairo_destroy(cr);

	cairo_surface_flush(surf);

	if (png_path) {
		cairo_surface_write_to_png(surf, png_path);
	} else if (raw_path) {
		uint8_t *src = cairo_image_surface_get_data(surf);
		int stride = cairo_image_surface_get_stride(surf);
		FILE *f = fopen(raw_path, "wb");
		if (f) {
			for (uint32_t r = 0; r < height; r++) {
				fwrite(src + r * stride, 1, width * 4, f);
			}
			fclose(f);
		}
	} else if (fbp) {
		uint8_t *src = cairo_image_surface_get_data(surf);
		int stride = cairo_image_surface_get_stride(surf);
		for (uint32_t r = 0; r < height; r++) {
			memcpy(fbp + r * finfo.line_length, src + r * stride, width * 4);
		}
		munmap(fbp, screensize);
	}

	cairo_surface_destroy(surf);
	if (fb_fd >= 0) {
		close(fb_fd);
	}

	if (!png_path) {
		int tty = open("/dev/tty0", O_WRONLY);
		if (tty >= 0) {
			/* Hide cursor and clear text */
			write(tty, "\033[?25l", 6);
			close(tty);
		}
	}
	return 0;
}
