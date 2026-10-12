
#ifndef JACK_CPC_DSK_OVERLAY_H
#define JACK_CPC_DSK_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t offset;
    uint8_t value;
} jack_cpc_dsk_patch;

typedef struct {
    const uint8_t *base;
    size_t base_size;
    jack_cpc_dsk_patch *patches;
    size_t patch_count;
    size_t patch_capacity;
    uint32_t write_generation;
} jack_cpc_dsk_overlay;

bool jack_cpc_dsk_overlay_init(jack_cpc_dsk_overlay *overlay, const void *base, size_t base_size,
                               jack_cpc_dsk_patch *patches, size_t patch_capacity);
bool jack_cpc_dsk_overlay_read(const jack_cpc_dsk_overlay *overlay, size_t offset, uint8_t *value);
bool jack_cpc_dsk_overlay_write(jack_cpc_dsk_overlay *overlay, size_t offset, uint8_t value);
size_t jack_cpc_dsk_overlay_export(const jack_cpc_dsk_overlay *overlay, uint8_t *output,
                                   size_t capacity);
bool jack_cpc_dsk_overlay_import(jack_cpc_dsk_overlay *overlay, const uint8_t *input, size_t bytes);
uint32_t jack_cpc_dsk_overlay_hash(const jack_cpc_dsk_overlay *overlay);

#endif
