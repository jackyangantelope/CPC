// SPDX-License-Identifier: BSD-3-Clause
#ifndef JACK_CPC_VIDEO_H
#define JACK_CPC_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
    uint16_t rgb555;
    uint8_t intensity;
} jack_video_vector;

bool jack_video_blit_1bpp(uint16_t *dst, uint16_t dst_width, uint16_t dst_height,
                          uint16_t dst_stride, const uint8_t *src, uint16_t src_width,
                          uint16_t src_height, uint16_t src_stride_bytes,
                          uint16_t foreground, uint16_t background);
bool jack_video_blit_index8(uint16_t *dst, uint16_t dst_width, uint16_t dst_height,
                            uint16_t dst_stride, const uint8_t *src, uint16_t src_width,
                            uint16_t src_height, uint16_t src_stride_bytes,
                            const uint16_t *palette, size_t palette_size);
bool jack_video_blit_rgb565(uint16_t *dst, uint16_t dst_width, uint16_t dst_height,
                            uint16_t dst_stride, const uint16_t *src, uint16_t src_width,
                            uint16_t src_height, uint16_t src_stride);
bool jack_video_blit_rgb555(uint16_t *dst, uint16_t dst_width, uint16_t dst_height,
                            uint16_t dst_stride, const uint16_t *src, uint16_t src_width,
                            uint16_t src_height, uint16_t src_stride);
bool jack_video_draw_vectors(uint16_t *dst, uint16_t width, uint16_t height,
                             uint16_t stride, const jack_video_vector *vectors,
                             size_t vector_count, int16_t coordinate_max);

#endif
