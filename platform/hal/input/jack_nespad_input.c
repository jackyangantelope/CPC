// SPDX-License-Identifier: BSD-3-Clause
#include "jack_nespad_input.h"

bool jack_nespad_input_init(jack_input_devices *devices, uint32_t cpu_khz) {
    (void)devices;
    (void)cpu_khz;
    return false;
}

bool jack_nespad_input_poll(void) {
    return false;
}
