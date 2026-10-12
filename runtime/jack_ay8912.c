
#include "jack_ay8912.h"

#include <string.h>

#define AY_CLOCK_HZ 1000000u

static const int16_t volume_table[16] = {
    0,64,96,136,192,272,384,544,768,1088,1536,2176,3072,4352,6144,8192
};

void jack_ay8912_reset(jack_ay8912 *ay) {
    if(ay == NULL) return;
    memset(ay, 0, sizeof(*ay));
    ay->noise_lfsr = 1u;
    ay->envelope_count = 15;
}

void jack_ay8912_register_written(jack_ay8912 *ay, uint8_t index, uint8_t value) {
    if(ay == NULL || index != 13u) return;
    ay->envelope_phase = 0;
    ay->envelope_count = 15;
    ay->envelope_attack = (value & 0x04u) != 0u ? 15u : 0u;
    if((value & 0x08u) == 0u) {
        ay->envelope_hold = 1u;
        ay->envelope_alternate = ay->envelope_attack != 0u;
    } else {
        ay->envelope_hold = value & 0x01u;
        ay->envelope_alternate = (value >> 1u) & 1u;
    }
    ay->envelope_holding = 0u;
}

static void clock_envelope(jack_ay8912 *ay) {
    if(ay->envelope_holding) return;
    ay->envelope_count--;
    if(ay->envelope_count >= 0) return;
    if(ay->envelope_hold) {
        if(ay->envelope_alternate) ay->envelope_attack ^= 15u;
        ay->envelope_holding = 1u;
        ay->envelope_count = 0;
    } else {
        if(ay->envelope_alternate) ay->envelope_attack ^= 15u;
        ay->envelope_count = 15;
    }
}

size_t jack_ay8912_render(jack_ay8912 *ay, const uint8_t registers[16],
                          jack_audio_ring *audio, size_t frames) {
    if(ay == NULL || registers == NULL || audio == NULL) return 0;
    size_t written = 0;
    for(; written < frames; ++written) {
        const uint8_t noise_period = (registers[6] & 0x1fu) != 0u ? registers[6] & 0x1fu : 1u;
        ay->noise_phase += AY_CLOCK_HZ;
        const uint32_t noise_limit = audio->sample_rate * 16u * noise_period;
        while(ay->noise_phase >= noise_limit) {
            ay->noise_phase -= noise_limit;
            const uint32_t feedback = (ay->noise_lfsr ^ (ay->noise_lfsr >> 3u)) & 1u;
            ay->noise_lfsr = (ay->noise_lfsr >> 1u) | (feedback << 16u);
        }

        const uint16_t envelope_period_raw = (uint16_t)(registers[11] | ((uint16_t)registers[12] << 8u));
        const uint16_t envelope_period = envelope_period_raw != 0u ? envelope_period_raw : 1u;
        ay->envelope_phase += AY_CLOCK_HZ;
        const uint64_t envelope_limit = (uint64_t)audio->sample_rate * 256u * envelope_period;
        while(ay->envelope_phase >= envelope_limit) {
            ay->envelope_phase -= envelope_limit;
            clock_envelope(ay);
        }

        int32_t mixed = 0;
        const uint8_t mixer = registers[7];
        for(unsigned channel = 0; channel < 3u; ++channel) {
            const uint16_t raw_period = (uint16_t)(registers[channel * 2u] |
                ((registers[channel * 2u + 1u] & 0x0fu) << 8u));
            const uint16_t period = raw_period != 0u ? raw_period : 1u;
            ay->tone_phase[channel] += AY_CLOCK_HZ;
            const uint32_t tone_limit = audio->sample_rate * 16u * period;
            while(ay->tone_phase[channel] >= tone_limit) ay->tone_phase[channel] -= tone_limit;
            const bool tone_high = (mixer & (1u << channel)) != 0u ||
                ay->tone_phase[channel] < tone_limit / 2u;
            const bool noise_high = (mixer & (1u << (channel + 3u))) != 0u ||
                (ay->noise_lfsr & 1u) != 0u;
            const uint8_t amplitude = registers[8u + channel];
            const uint8_t level = (amplitude & 0x10u) != 0u ?
                (uint8_t)(ay->envelope_count ^ ay->envelope_attack) : amplitude & 0x0fu;
            mixed += tone_high && noise_high ? volume_table[level] : -volume_table[level];
        }
        mixed /= 3;
        if(!jack_audio_push(audio, (jack_audio_frame){(int16_t)mixed, (int16_t)mixed})) break;
    }
    return written;
}
