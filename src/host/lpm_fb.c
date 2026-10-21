#include "lpm_fb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void touch(lpm_fb *fb, int x, int y, int w, int h)
{
    fb->calls++;
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > LPM_SCREEN_W || y + h > LPM_SCREEN_H) {
        fb->out_of_bounds = true;
        return;
    }
    fb->pixels += (uint32_t)(w * h);
    if (h && y < fb->min_y) {
        fb->min_y = y;
    }
    if (h && y + h - 1 > fb->max_y) {
        fb->max_y = y + h - 1;
    }
}

static void fb_fill(void *ctx, int16_t x, int16_t y, int16_t w, int16_t h, lpm_color color)
{
    lpm_fb *fb = ctx;
    int row, col;
    touch(fb, x, y, w, h);
    if (fb->out_of_bounds) {
        return;
    }
    for (row = y; row < y + h; row++) {
        for (col = x; col < x + w; col++) {
            fb->px[row * LPM_SCREEN_W + col] = color;
        }
    }
}

static void fb_mono(void *ctx, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *bits, uint16_t stride,
                    lpm_color fg, lpm_color bg)
{
    lpm_fb *fb = ctx;
    int row, col;
    touch(fb, x, y, w, h);
    if (fb->out_of_bounds) {
        return;
    }
    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            bool on = (bits[row * stride + col / 8] >> (7 - col % 8)) & 1;
            fb->px[(y + row) * LPM_SCREEN_W + x + col] = on ? fg : bg;
        }
    }
}

lpm_display lpm_fb_display(lpm_fb *fb)
{
    lpm_display d;
    d.ctx = fb;
    d.fill_rect = fb_fill;
    d.draw_mono = fb_mono;
    lpm_fb_reset_stats(fb);
    return d;
}

void lpm_fb_reset_stats(lpm_fb *fb)
{
    fb->calls = 0;
    fb->pixels = 0;
    fb->min_y = LPM_SCREEN_H;
    fb->max_y = -1;
}

/* ---- Minimal PNG encoder (stored deflate blocks, no zlib dependency) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t n)
{
    size_t i;
    if (!crc_table[1]) {
        uint32_t c, k;
        for (k = 0; k < 256; k++) {
            int j;
            for (c = k, j = 0; j < 8; j++) {
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            crc_table[k] = c;
        }
    }
    for (i = 0; i < n; i++) {
        crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t head[8];
    uint8_t tail[4];
    uint32_t crc;
    put32(head, (uint32_t)n);
    memcpy(head + 4, type, 4);
    crc = crc32_update(0xffffffffu, head + 4, 4);
    crc = crc32_update(crc, data, n) ^ 0xffffffffu;
    put32(tail, crc);
    return fwrite(head, 1, 8, f) == 8 && fwrite(data, 1, n, f) == n && fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

static int write_rgb_png(const char *path, int width, int height, const uint8_t *rgb)
{
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    size_t row_bytes = (size_t)width * 3 + 1;
    size_t raw_len = row_bytes * (size_t)height;
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *raw = malloc(raw_len);
    uint8_t *z = malloc(z_len);
    uint8_t ihdr[13];
    uint32_t a = 1, b = 0;
    size_t i, pos = 0;
    FILE *f;
    int rc = -1;

    if (!raw || !z) {
        goto done;
    }
    for (i = 0; i < (size_t)height; i++) {
        raw[i * row_bytes] = 0;
        memcpy(raw + i * row_bytes + 1, rgb + i * (size_t)width * 3, (size_t)width * 3);
    }
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    for (i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        z[pos++] = i + n == raw_len;
        z[pos++] = (uint8_t)n;
        z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)~n;
        z[pos++] = (uint8_t)(~n >> 8);
        memcpy(z + pos, raw + i, n);
        pos += n;
    }
    for (i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    put32(z + pos, (b << 16) | a);
    pos += 4;

    put32(ihdr, (uint32_t)width);
    put32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    f = fopen(path, "wb");
    if (f) {
        rc = fwrite(signature, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) == 0 &&
                     write_chunk(f, "IDAT", z, pos) == 0 && write_chunk(f, "IEND", NULL, 0) == 0
                 ? 0
                 : -1;
        if (fclose(f) != 0) {
            rc = -1;
        }
    }
done:
    free(raw);
    free(z);
    return rc;
}

static void put_pixel(uint8_t *rgb, int stride_px, int x, int y, lpm_color c)
{
    uint8_t *p = rgb + ((size_t)y * stride_px + x) * 3;
    p[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    p[2] = (uint8_t)((c & 31) * 255 / 31);
}

int lpm_fb_write_sheet(const lpm_fb *const *fbs, int count, int columns, const char *path, int scale)
{
    const int gap = 12;
    int rows = (count + columns - 1) / columns;
    int cell_w = LPM_SCREEN_W * scale + gap;
    int cell_h = LPM_SCREEN_H * scale + gap;
    int width = columns * cell_w + gap;
    int height = rows * cell_h + gap;
    uint8_t *rgb = malloc((size_t)width * height * 3);
    int i, x, y;
    int rc;
    if (!rgb) {
        return -1;
    }
    memset(rgb, 0x30, (size_t)width * height * 3);
    for (i = 0; i < count; i++) {
        int ox = gap + (i % columns) * cell_w;
        int oy = gap + (i / columns) * cell_h;
        for (y = 0; y < LPM_SCREEN_H * scale; y++) {
            for (x = 0; x < LPM_SCREEN_W * scale; x++) {
                put_pixel(rgb, width, ox + x, oy + y, fbs[i]->px[(y / scale) * LPM_SCREEN_W + x / scale]);
            }
        }
    }
    rc = write_rgb_png(path, width, height, rgb);
    free(rgb);
    return rc;
}

int lpm_fb_write_png(const lpm_fb *fb, const char *path, int scale)
{
    int width = LPM_SCREEN_W * scale;
    int height = LPM_SCREEN_H * scale;
    uint8_t *rgb = malloc((size_t)width * height * 3);
    int x, y, rc;
    if (!rgb) {
        return -1;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            put_pixel(rgb, width, x, y, fb->px[(y / scale) * LPM_SCREEN_W + x / scale]);
        }
    }
    rc = write_rgb_png(path, width, height, rgb);
    free(rgb);
    return rc;
}
