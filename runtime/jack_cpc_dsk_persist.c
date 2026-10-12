
#include "jack_cpc_dsk_persist.h"

#include <string.h>

static void put32(uint8_t *out, uint32_t value) {
    for(unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8u * i));
}

static uint32_t get32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8u) |
           ((uint32_t)in[2] << 16u) | ((uint32_t)in[3] << 24u);
}

bool jack_cpc_dsk_overlay_save_stream(const jack_cpc_dsk_overlay *overlay, jack_stream *stream) {
    if(overlay == NULL || stream == NULL || overlay->patch_count > UINT32_MAX ||
       !jack_stream_seek(stream, 0u)) return false;
    uint8_t header[16];
    memcpy(header, "JCOW", 4);
    put32(header + 4, 1u);
    put32(header + 8, (uint32_t)overlay->patch_count);
    put32(header + 12, jack_cpc_dsk_overlay_hash(overlay));
    if(!jack_stream_write_exact(stream, header, sizeof(header))) return false;
    for(size_t i = 0; i < overlay->patch_count; ++i) {
        uint8_t record[5];
        put32(record, overlay->patches[i].offset);
        record[4] = overlay->patches[i].value;
        if(!jack_stream_write_exact(stream, record, sizeof(record))) return false;
    }
    return jack_stream_flush(stream);
}

bool jack_cpc_dsk_overlay_load_stream(jack_cpc_dsk_overlay *overlay, jack_stream *stream) {
    if(overlay == NULL || stream == NULL || !jack_stream_seek(stream, 0u)) return false;
    uint8_t header[16];
    if(!jack_stream_read_exact(stream, header, sizeof(header)) || memcmp(header, "JCOW", 4) != 0 ||
       get32(header + 4) != 1u) return false;
    const uint32_t count = get32(header + 8);
    const uint64_t expected = 16u + (uint64_t)count * 5u;
    if(count > overlay->patch_capacity || jack_stream_size(stream) != expected) return false;
    overlay->patch_count = 0;
    overlay->write_generation = 0u;
    for(uint32_t i = 0; i < count; ++i) {
        uint8_t record[5];
        if(!jack_stream_read_exact(stream, record, sizeof(record))) {
            overlay->patch_count = 0;
            return false;
        }
        const uint32_t offset = get32(record);
        if(offset >= overlay->base_size) {
            overlay->patch_count = 0;
            return false;
        }
        overlay->patches[overlay->patch_count++] = (jack_cpc_dsk_patch){offset, record[4]};
    }
    if(jack_cpc_dsk_overlay_hash(overlay) != get32(header + 12)) {
        overlay->patch_count = 0;
        return false;
    }
    overlay->write_generation = 0u;
    return true;
}
