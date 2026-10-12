
#ifndef JACK_AY8912_H
#define JACK_AY8912_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jack_audio.h"

typedef struct {
    uint32_t tone_phase[3];
    uint32_t noise_phase;
    uint64_t envelope_phase;
    uint32_t noise_lfsr;
    int8_t envelope_count;
    uint8_t envelope_attack;
    uint8_t envelope_hold;
    uint8_t envelope_alternate;
    uint8_t envelope_holding;
} jack_ay8912;

void jack_ay8912_reset(jack_ay8912 *ay);
void jack_ay8912_register_written(jack_ay8912 *ay, uint8_t index, uint8_t value);
size_t jack_ay8912_render(jack_ay8912 *ay, const uint8_t registers[16],
                          jack_audio_ring *audio, size_t frames);

#endif
