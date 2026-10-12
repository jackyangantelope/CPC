// SPDX-License-Identifier: BSD-3-Clause
#include "jack_video.h"

#include <stdlib.h>

static bool valid_surface(const uint16_t *dst, uint16_t dw, uint16_t dh, uint16_t ds,
                          const void *src, uint16_t sw, uint16_t sh) {
    return dst != NULL && src != NULL && dw != 0u && dh != 0u && sw != 0u && sh != 0u &&
           ds >= dw;
}

static uint16_t scale_intensity(uint16_t color, uint8_t intensity) {
    const uint32_t r = ((color >> 10) & 31u) * intensity / 255u;
    const uint32_t g = ((color >> 5) & 31u) * intensity / 255u;
    const uint32_t b = (color & 31u) * intensity / 255u;
    return (uint16_t)((r << 10) | (g << 5) | b);
}

bool jack_video_blit_1bpp(uint16_t *dst, uint16_t dw, uint16_t dh, uint16_t ds,
                          const uint8_t *src, uint16_t sw, uint16_t sh, uint16_t ss,
                          uint16_t foreground, uint16_t background) {
    if(!valid_surface(dst, dw, dh, ds, src, sw, sh) || ss < (uint16_t)((sw + 7u) / 8u)) {
        return false;
    }
    for(uint16_t y = 0; y < dh; ++y) {
        const uint32_t sy = (uint32_t)y * sh / dh;
        for(uint16_t x = 0; x < dw; ++x) {
            const uint32_t sx = (uint32_t)x * sw / dw;
            const uint8_t bits = src[sy * ss + sx / 8u];
            dst[(size_t)y * ds + x] = (bits & (uint8_t)(0x80u >> (sx & 7u))) != 0u
                                             ? foreground : background;
        }
    }
    return true;
}

bool jack_video_blit_index8(uint16_t *dst, uint16_t dw, uint16_t dh, uint16_t ds,
                            const uint8_t *src, uint16_t sw, uint16_t sh, uint16_t ss,
                            const uint16_t *palette, size_t palette_size) {
    if(!valid_surface(dst, dw, dh, ds, src, sw, sh) || ss < sw || palette == NULL ||
       palette_size == 0u || palette_size > 256u) {
        return false;
    }
    for(uint16_t y = 0; y < dh; ++y) {
        const uint32_t sy = (uint32_t)y * sh / dh;
        for(uint16_t x = 0; x < dw; ++x) {
            const uint32_t sx = (uint32_t)x * sw / dw;
            const uint8_t index = src[sy * ss + sx];
            dst[(size_t)y * ds + x] = index < palette_size ? palette[index] : 0u;
        }
    }
    return true;
}

bool jack_video_blit_rgb565(uint16_t *dst, uint16_t dw, uint16_t dh, uint16_t ds,
                            const uint16_t *src, uint16_t sw, uint16_t sh, uint16_t ss) {
    if(!valid_surface(dst, dw, dh, ds, src, sw, sh) || ss < sw) {
        return false;
    }
    for(uint16_t y = 0; y < dh; ++y) {
        const uint32_t sy = (uint32_t)y * sh / dh;
        for(uint16_t x = 0; x < dw; ++x) {
            const uint32_t sx = (uint32_t)x * sw / dw;
            const uint16_t value = src[sy * ss + sx];
            dst[(size_t)y * ds + x] =
                (uint16_t)(((value & 0xf800u) >> 1) | ((value & 0x07c0u) >> 1) |
                           (value & 0x001fu));
        }
    }
    return true;
}

bool jack_video_blit_rgb555(uint16_t *dst, uint16_t dw, uint16_t dh, uint16_t ds,
                            const uint16_t *src, uint16_t sw, uint16_t sh, uint16_t ss) {
    if(!valid_surface(dst, dw, dh, ds, src, sw, sh) || ss < sw) {
        return false;
    }
    for(uint16_t y = 0; y < dh; ++y) {
        const uint32_t sy = (uint32_t)y * sh / dh;
        for(uint16_t x = 0; x < dw; ++x) {
            const uint32_t sx = (uint32_t)x * sw / dw;
            dst[(size_t)y * ds + x] = (uint16_t)(src[sy * ss + sx] & 0x7fffu);
        }
    }
    return true;
}

static void draw_line(uint16_t *dst, uint16_t width, uint16_t height, uint16_t stride,
                      int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for(;;) {
        if(x0 >= 0 && y0 >= 0 && x0 < width && y0 < height) {
            dst[(size_t)y0 * stride + (size_t)x0] = color;
        }
        if(x0 == x1 && y0 == y1) {
            break;
        }
        const int twice = error * 2;
        if(twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if(twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

bool jack_video_draw_vectors(uint16_t *dst, uint16_t width, uint16_t height,
                             uint16_t stride, const jack_video_vector *vectors,
                             size_t vector_count, int16_t coordinate_max) {
    if(dst == NULL || vectors == NULL || width == 0u || height == 0u || stride < width ||
       coordinate_max <= 0) {
        return false;
    }
    for(size_t index = 0; index < vector_count; ++index) {
        const jack_video_vector *v = &vectors[index];
        const int x0 = (int)((int32_t)v->x0 * (width - 1u) / coordinate_max);
        const int y0 = (int)((int32_t)v->y0 * (height - 1u) / coordinate_max);
        const int x1 = (int)((int32_t)v->x1 * (width - 1u) / coordinate_max);
        const int y1 = (int)((int32_t)v->y1 * (height - 1u) / coordinate_max);
        draw_line(dst, width, height, stride, x0, y0, x1, y1,
                  scale_intensity(v->rgb555, v->intensity));
    }
    return true;
}
