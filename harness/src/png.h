/* png.h - minimal PNG writers (RGB8 with stored deflate; indexed with zlib). */
#pragma once
#include <stdint.h>

/* rgb: width*height*3 bytes. returns 0 on success (stored deflate, no zlib) */
int png_write_rgb(const char *path, const uint8_t *rgb, int width, int height);
/* 8-bit palette image (colour type 3), compressed with the system zlib;
   idx: width*height palette indices, pal: 0xRRGGBB entries */
int png_write_indexed(const char *path, const uint8_t *idx, int width, int height,
                      const uint32_t *pal, int ncolors);
