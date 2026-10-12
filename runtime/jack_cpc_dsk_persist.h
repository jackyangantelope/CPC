
#ifndef JACK_CPC_DSK_PERSIST_H
#define JACK_CPC_DSK_PERSIST_H

#include "jack_cpc_dsk_overlay.h"
#include "jack_stream.h"

bool jack_cpc_dsk_overlay_save_stream(const jack_cpc_dsk_overlay *overlay, jack_stream *stream);
bool jack_cpc_dsk_overlay_load_stream(jack_cpc_dsk_overlay *overlay, jack_stream *stream);

#endif
