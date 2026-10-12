// SPDX-License-Identifier: GPL-3.0-only
#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef VIDEO_MODE_320x240

#define MODE_H_FRONT_PORCH 32
#define MODE_H_SYNC_WIDTH 192
#define MODE_H_BACK_PORCH 96
#define MODE_H_ACTIVE_PIXELS 1280

#define MODE_V_FRONT_PORCH 4
#define MODE_V_SYNC_WIDTH 4
#define MODE_V_BACK_PORCH 14
#define MODE_V_ACTIVE_LINES 240

#define MODE_HSTX_CLK_DIV 1
#define MODE_HSTX_CSR_CLKDIV 5

#else

#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480

#define MODE_HSTX_CLK_DIV 1
#define MODE_HSTX_CSR_CLKDIV 5

#endif

#define MODE_H_TOTAL_PIXELS (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

extern uint16_t frame_width;
extern uint16_t frame_height;

extern volatile uint32_t video_frame_count;

typedef void (*video_output_task_fn)(void);
typedef void (*video_output_vsync_cb_t)(void);

typedef void (*video_output_scanline_cb_t)(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer);

void video_output_init(uint16_t width, uint16_t height);

void video_output_stop(void);

void video_output_set_scanline_callback(video_output_scanline_cb_t cb);

void video_output_set_vsync_callback(video_output_vsync_cb_t cb);

void video_output_set_background_task(video_output_task_fn task);

bool video_output_get_dvi_mode(void);

void video_output_set_dvi_mode(bool enabled);

void video_output_core1_run(void);

void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate);

uint32_t pico_hdmi_get_audio_sample_rate(void);

void video_output_request_resync(void);

int get_video_output_resync_count(void);

#endif
