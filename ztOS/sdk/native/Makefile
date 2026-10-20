CC ?= cc
ARCH ?= $(shell uname -m)
OUT = prebuilt/$(ARCH)/libztos-render.so
SHARED = ../../ztmetro/core/zrnmetrorender.h
all: $(OUT)
$(OUT): renderer.c $(SHARED)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared -I../../ztmetro/core $(shell pkg-config --cflags glesv2) -o $@ renderer.c $(shell pkg-config --libs glesv2) -lm
	cat renderer.c $(SHARED) | sha256sum | cut -d " " -f1 > $(OUT:.so=.sha256)
clean:
	rm -rf prebuilt
.PHONY: all clean
