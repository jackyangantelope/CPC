// SPDX-License-Identifier: BSD-3-Clause
#ifndef JACK_TERMINAL_RGB555_H
#define JACK_TERMINAL_RGB555_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jack_terminal.h"

#define JACK_TERMINAL_CELL_WIDTH 8u
#define JACK_TERMINAL_CELL_HEIGHT 8u

extern const uint16_t jack_rgb555_palette[16];

bool jack_terminal_render_rgb555(const jack_terminal *terminal,
                                 uint16_t *framebuffer,
                                 uint16_t pixel_width,
                                 uint16_t pixel_height,
                                 uint16_t stride_pixels,
                                 const uint16_t palette[16]);

uint32_t jack_rgb555_hash(const uint16_t *framebuffer, size_t pixel_count);

#endif
