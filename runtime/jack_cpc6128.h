// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef JACK_CPC_CPC6128_H
#define JACK_CPC_CPC6128_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jack_audio.h"
#include "jack_ay8912.h"
#include "jack_cpc_dsk_overlay.h"
#include "jack_input.h"
#include "jack_stream.h"

#define JACK_CPC_RAM_BYTES       (128u * 1024u)
#define JACK_CPC_ROM_BYTES       (16u * 1024u)
#define JACK_CPC_FRAME_WIDTH     320u
#define JACK_CPC_FRAME_HEIGHT    240u
#define JACK_CPC_FRAME_BYTES     (JACK_CPC_FRAME_WIDTH * JACK_CPC_FRAME_HEIGHT)
#define JACK_CPC_AUDIO_RATE      44100u

typedef enum {
    JACK_CPC_ROM_LOWER = 0,
    JACK_CPC_ROM_BASIC,
    JACK_CPC_ROM_AMSDOS
} jack_cpc_rom_kind;

typedef struct {
    uint8_t *ram;
    size_t ram_size;
    uint8_t *lower_rom;
    uint8_t *basic_rom;
    uint8_t *amsdos_rom;
    uint8_t *frame_index8;
    size_t frame_stride;
    jack_audio_ring *audio;
} jack_cpc6128_config;

typedef struct {
    jack_cpc6128_config config;
    bool rom_loaded[3];
    const uint8_t *dsk_data;
    size_t dsk_size;
    uint8_t dsk_tracks;
    uint8_t dsk_sides;
    bool dsk_extended;
    bool dsk_mounted;
    uint8_t keyboard_matrix[16];
    uint32_t crtc_ticks;
    uint32_t frame_count;
    uint32_t interrupt_count;
    uint32_t io_reads;
    uint32_t io_writes;
    jack_ay8912 ay;
    jack_cpc_dsk_overlay *dsk_overlay;
    uint8_t selected_upper_rom;
    uint8_t current_mode;

    uint8_t scanline_inks[312][17];
    uint16_t frame_source_y[240];
    uint32_t video_us;
    uint32_t audio_us;
} jack_cpc6128;

bool jack_cpc6128_init(jack_cpc6128 *machine, const jack_cpc6128_config *config);
bool jack_cpc6128_load_rom(jack_cpc6128 *machine, jack_cpc_rom_kind kind,
                           jack_stream *stream);
bool jack_cpc6128_load_system_rom(jack_cpc6128 *machine, jack_stream *stream);
bool jack_cpc6128_mount_dsk(jack_cpc6128 *machine, const void *data, size_t size);
bool jack_cpc6128_attach_dsk_overlay(jack_cpc6128 *machine,
                                     jack_cpc_dsk_overlay *overlay);
void jack_cpc6128_eject_dsk(jack_cpc6128 *machine);
void jack_cpc6128_reset(jack_cpc6128 *machine);
int jack_cpc6128_run_frame(jack_cpc6128 *machine);
void jack_cpc6128_set_matrix_key(jack_cpc6128 *machine, uint8_t row, uint8_t bit,
                                 bool pressed);
bool jack_cpc6128_type_ascii(jack_cpc6128 *machine, uint16_t code, bool pressed);
bool jack_cpc6128_handle_input(jack_cpc6128 *machine, const jack_input_event *event);
void jack_cpc6128_palette_rgb555(const jack_cpc6128 *machine, uint16_t palette[17]);
void jack_cpc6128_scanline_palette_rgb555(const jack_cpc6128 *machine,
                                         unsigned y,uint16_t palette[17]);
uint32_t jack_cpc6128_frame_hash(const jack_cpc6128 *machine);

#endif
