
#include "jack_cpc_dsk_overlay.h"

#include <string.h>

static uint32_t fnv_update(uint32_t hash, uint8_t value) {
    return (hash ^ value) * 16777619u;
}

static void put32(uint8_t *output, uint32_t value) {
    for(unsigned i = 0; i < 4; ++i) output[i] = (uint8_t)(value >> (i * 8u));
}

static uint32_t get32(const uint8_t *input) {
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8u) |
           ((uint32_t)input[2] << 16u) | ((uint32_t)input[3] << 24u);
}

bool jack_cpc_dsk_overlay_init(jack_cpc_dsk_overlay *overlay, const void *base, size_t base_size,
                               jack_cpc_dsk_patch *patches, size_t patch_capacity) {
    if(overlay == NULL || base == NULL || base_size == 0 || patches == NULL || patch_capacity == 0)
        return false;
    overlay->base = (const uint8_t *)base;
    overlay->base_size = base_size;
    overlay->patches = patches;
    overlay->patch_count = 0;
    overlay->patch_capacity = patch_capacity;
    overlay->write_generation = 0u;

    return true;
}

bool jack_cpc_dsk_overlay_read(const jack_cpc_dsk_overlay *overlay, size_t offset, uint8_t *value) {
    if(overlay == NULL || value == NULL || offset >= overlay->base_size) return false;
    for(size_t i = overlay->patch_count; i > 0; --i) {
        if(overlay->patches[i - 1u].offset == offset) {
            *value = overlay->patches[i - 1u].value;
            return true;
        }
    }
    *value = overlay->base[offset];

    return true;
}

bool jack_cpc_dsk_overlay_write(jack_cpc_dsk_overlay *overlay, size_t offset, uint8_t value) {
    if(overlay == NULL || offset >= overlay->base_size) return false;
    for(size_t i = 0; i < overlay->patch_count; ++i) {
        if(overlay->patches[i].offset == offset) {
            if(overlay->patches[i].value != value) {
                overlay->patches[i].value = value;
                ++overlay->write_generation;
            }
            return true;
        }
    }
    if(overlay->patch_count >= overlay->patch_capacity || offset > UINT32_MAX) return false;
    overlay->patches[overlay->patch_count++] = (jack_cpc_dsk_patch){(uint32_t)offset, value};
    ++overlay->write_generation;
    return true;
}

uint32_t jack_cpc_dsk_overlay_hash(const jack_cpc_dsk_overlay *overlay) {
    if(overlay == NULL) return 0;
    uint32_t hash = 2166136261u;
    for(size_t i = 0; i < overlay->patch_count; ++i) {
        uint32_t offset = overlay->patches[i].offset;
        for(unsigned byte = 0; byte < 4; ++byte) {
            hash = fnv_update(hash, (uint8_t)offset);
            offset >>= 8u;
        }
        hash = fnv_update(hash, overlay->patches[i].value);
    }
    return hash;
}

size_t jack_cpc_dsk_overlay_export(const jack_cpc_dsk_overlay *overlay, uint8_t *output,
                                   size_t capacity) {
    if(overlay == NULL || output == NULL || overlay->patch_count > UINT32_MAX) return 0;
    const size_t needed = 16u + overlay->patch_count * 5u;
    if(capacity < needed) return 0;
    memcpy(output, "JCOW", 4);
    put32(output + 4, 1u);
    put32(output + 8, (uint32_t)overlay->patch_count);
    put32(output + 12, jack_cpc_dsk_overlay_hash(overlay));
    for(size_t i = 0; i < overlay->patch_count; ++i) {
        put32(output + 16u + i * 5u, overlay->patches[i].offset);
        output[20u + i * 5u] = overlay->patches[i].value;
    }
    return needed;
}

bool jack_cpc_dsk_overlay_import(jack_cpc_dsk_overlay *overlay, const uint8_t *input, size_t bytes) {
    if(overlay == NULL || input == NULL || bytes < 16u || memcmp(input, "JCOW", 4) != 0 ||
       get32(input + 4) != 1u) return false;
    const uint32_t count = get32(input + 8);
    if(count > overlay->patch_capacity || bytes != 16u + (size_t)count * 5u) return false;
    overlay->patch_count = 0;
    overlay->write_generation = 0u;
    for(uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = get32(input + 16u + i * 5u);
        if(offset >= overlay->base_size) {
            overlay->patch_count = 0;
            return false;
        }
        overlay->patches[overlay->patch_count++] =
            (jack_cpc_dsk_patch){offset, input[20u + i * 5u]};
    }
    if(jack_cpc_dsk_overlay_hash(overlay) != get32(input + 12)) {
        overlay->patch_count = 0;
        return false;
    }
    overlay->write_generation = 0u;
    return true;
}
