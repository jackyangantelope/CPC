// SPDX-License-Identifier: BSD-3-Clause
#include "jack_terminal_rgb555.h"

#define RGB555(r, g, b) ((uint16_t)(((r) << 10) | ((g) << 5) | (b)))
#define GLYPH7(a, b, c, d, e, f, g) \
    (((uint64_t)(a) << 30) | ((uint64_t)(b) << 25) | ((uint64_t)(c) << 20) | \
     ((uint64_t)(d) << 15) | ((uint64_t)(e) << 10) | ((uint64_t)(f) << 5) | (g))

const uint16_t jack_rgb555_palette[16] = {
    RGB555(0, 0, 0), RGB555(0, 0, 21), RGB555(0, 21, 0), RGB555(0, 21, 21),
    RGB555(21, 0, 0), RGB555(21, 0, 21), RGB555(21, 10, 0), RGB555(21, 21, 21),
    RGB555(10, 10, 10), RGB555(10, 10, 31), RGB555(10, 31, 10), RGB555(10, 31, 31),
    RGB555(31, 10, 10), RGB555(31, 10, 31), RGB555(31, 31, 10), RGB555(31, 31, 31),
};

static uint64_t glyph_pattern(uint8_t glyph) {
    if(glyph >= 'a' && glyph <= 'z') {
        glyph = (uint8_t)(glyph - ('a' - 'A'));
    }
    switch(glyph) {
    case 'A': return GLYPH7(14, 17, 17, 31, 17, 17, 17);
    case 'B': return GLYPH7(30, 17, 17, 30, 17, 17, 30);
    case 'C': return GLYPH7(14, 17, 16, 16, 16, 17, 14);
    case 'D': return GLYPH7(30, 17, 17, 17, 17, 17, 30);
    case 'E': return GLYPH7(31, 16, 16, 30, 16, 16, 31);
    case 'F': return GLYPH7(31, 16, 16, 30, 16, 16, 16);
    case 'G': return GLYPH7(14, 17, 16, 23, 17, 17, 15);
    case 'H': return GLYPH7(17, 17, 17, 31, 17, 17, 17);
    case 'I': return GLYPH7(14, 4, 4, 4, 4, 4, 14);
    case 'J': return GLYPH7(7, 2, 2, 2, 2, 18, 12);
    case 'K': return GLYPH7(17, 18, 20, 24, 20, 18, 17);
    case 'L': return GLYPH7(16, 16, 16, 16, 16, 16, 31);
    case 'M': return GLYPH7(17, 27, 21, 21, 17, 17, 17);
    case 'N': return GLYPH7(17, 25, 21, 19, 17, 17, 17);
    case 'O': return GLYPH7(14, 17, 17, 17, 17, 17, 14);
    case 'P': return GLYPH7(30, 17, 17, 30, 16, 16, 16);
    case 'Q': return GLYPH7(14, 17, 17, 17, 21, 18, 13);
    case 'R': return GLYPH7(30, 17, 17, 30, 20, 18, 17);
    case 'S': return GLYPH7(15, 16, 16, 14, 1, 1, 30);
    case 'T': return GLYPH7(31, 4, 4, 4, 4, 4, 4);
    case 'U': return GLYPH7(17, 17, 17, 17, 17, 17, 14);
    case 'V': return GLYPH7(17, 17, 17, 17, 17, 10, 4);
    case 'W': return GLYPH7(17, 17, 17, 21, 21, 21, 10);
    case 'X': return GLYPH7(17, 17, 10, 4, 10, 17, 17);
    case 'Y': return GLYPH7(17, 17, 10, 4, 4, 4, 4);
    case 'Z': return GLYPH7(31, 1, 2, 4, 8, 16, 31);
    case '0': return GLYPH7(14, 17, 19, 21, 25, 17, 14);
    case '1': return GLYPH7(4, 12, 4, 4, 4, 4, 14);
    case '2': return GLYPH7(14, 17, 1, 2, 4, 8, 31);
    case '3': return GLYPH7(30, 1, 1, 14, 1, 1, 30);
    case '4': return GLYPH7(2, 6, 10, 18, 31, 2, 2);
    case '5': return GLYPH7(31, 16, 16, 30, 1, 1, 30);
    case '6': return GLYPH7(14, 16, 16, 30, 17, 17, 14);
    case '7': return GLYPH7(31, 1, 2, 4, 8, 8, 8);
    case '8': return GLYPH7(14, 17, 17, 14, 17, 17, 14);
    case '9': return GLYPH7(14, 17, 17, 15, 1, 1, 14);
    case '-': return GLYPH7(0, 0, 0, 31, 0, 0, 0);
    case '_': return GLYPH7(0, 0, 0, 0, 0, 0, 31);
    case '.': return GLYPH7(0, 0, 0, 0, 0, 12, 12);
    case ':': return GLYPH7(0, 12, 12, 0, 12, 12, 0);
    case '/': return GLYPH7(1, 2, 2, 4, 8, 8, 16);
    case '+': return GLYPH7(0, 4, 4, 31, 4, 4, 0);
    case '=': return GLYPH7(0, 0, 31, 0, 31, 0, 0);
    case '!': return GLYPH7(4, 4, 4, 4, 4, 0, 4);
    case '?': return GLYPH7(14, 17, 1, 2, 4, 0, 4);
    case '@': return GLYPH7(14, 17, 23, 21, 23, 16, 14);
    case '#': return GLYPH7(10, 31, 10, 10, 31, 10, 0);
    case '*': return GLYPH7(0, 21, 14, 31, 14, 21, 0);
    case '(': return GLYPH7(2, 4, 8, 8, 8, 4, 2);
    case ')': return GLYPH7(8, 4, 2, 2, 2, 4, 8);
    case '[': return GLYPH7(14, 8, 8, 8, 8, 8, 14);
    case ']': return GLYPH7(14, 2, 2, 2, 2, 2, 14);
    case '|': return GLYPH7(4, 4, 4, 4, 4, 4, 4);
    case '<': return GLYPH7(2, 4, 8, 16, 8, 4, 2);
    case '>': return GLYPH7(8, 4, 2, 1, 2, 4, 8);
    case ' ': return 0;
    default: return GLYPH7(31, 17, 1, 2, 4, 0, 4);
    }
}

bool jack_terminal_render_rgb555(const jack_terminal *terminal,
                                 uint16_t *framebuffer,
                                 uint16_t pixel_width,
                                 uint16_t pixel_height,
                                 uint16_t stride_pixels,
                                 const uint16_t palette[16]) {
    if(terminal == NULL || terminal->cells == NULL || framebuffer == NULL ||
       palette == NULL || stride_pixels < pixel_width ||
       (uint32_t)terminal->width * JACK_TERMINAL_CELL_WIDTH > pixel_width ||
       (uint32_t)terminal->height * JACK_TERMINAL_CELL_HEIGHT > pixel_height) {
        return false;
    }

    for(uint16_t cell_y = 0; cell_y < terminal->height; ++cell_y) {
        for(uint16_t cell_x = 0; cell_x < terminal->width; ++cell_x) {
            const jack_terminal_cell *cell =
                &terminal->cells[(size_t)cell_y * terminal->width + cell_x];
            const bool inverse = (cell->flags & 1u) != 0;
            const uint16_t foreground = palette[inverse ? cell->background & 15u : cell->foreground & 15u];
            const uint16_t background = palette[inverse ? cell->foreground & 15u : cell->background & 15u];
            const uint64_t pattern = glyph_pattern(cell->glyph);
            for(uint16_t py = 0; py < JACK_TERMINAL_CELL_HEIGHT; ++py) {
                uint16_t *row = framebuffer +
                    (size_t)(cell_y * JACK_TERMINAL_CELL_HEIGHT + py) * stride_pixels +
                    cell_x * JACK_TERMINAL_CELL_WIDTH;
                const uint8_t bits = py < 7u ? (uint8_t)((pattern >> ((6u - py) * 5u)) & 31u) : 0u;
                for(uint16_t px = 0; px < JACK_TERMINAL_CELL_WIDTH; ++px) {
                    const bool glyph_pixel = px >= 1u && px <= 5u &&
                        ((bits >> (5u - px)) & 1u) != 0;
                    const bool underline = py == 7u && (cell->flags & 2u) != 0;
                    row[px] = glyph_pixel || underline ? foreground : background;
                }
            }
        }
    }
    return true;
}

uint32_t jack_rgb555_hash(const uint16_t *framebuffer, size_t pixel_count) {
    uint32_t hash = 2166136261u;
    for(size_t i = 0; i < pixel_count; ++i) {
        hash ^= framebuffer[i] & 0xffu;
        hash *= 16777619u;
        hash ^= framebuffer[i] >> 8;
        hash *= 16777619u;
    }
    return hash;
}
