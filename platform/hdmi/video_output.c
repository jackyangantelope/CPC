// SPDX-License-Identifier: GPL-3.0-only
#include "video_output.h"

#include "hstx_data_island_queue.h"
#include "hstx_packet.h"
#include "hstx_pins.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/pll.h"

#include <math.h>
#include <string.h>
#include "hstx.h"
#include "stdio.h"

#ifndef HSTX_DEBUG
#define HSTX_DEBUG 0
#endif

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define PREAMBLE_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

#define VIDEO_PREAMBLE_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))
#define VIDEO_PREAMBLE_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))

#define VIDEO_GUARD_BAND (0x2CCu | (0x133u << 10) | (0x2CCu << 20))

#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

#define SYNC_AFTER_DI (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND)

#define W_VIDEO_PREAMBLE 8
#define W_VIDEO_GUARD_BAND 2

uint16_t frame_width = 0;
uint16_t frame_height = 0;
volatile uint32_t video_frame_count = 0;

#if HSTX_DEBUG
static volatile uint32_t irq_count = 0;
#endif

static bool dvi_mode = false;

#ifndef PICO_HDMI_DVI_USE_DATA_ISLANDS
#define PICO_HDMI_DVI_USE_DATA_ISLANDS 0
#endif

static uint16_t line_buffer[2][MODE_H_ACTIVE_PIXELS] __attribute__((aligned(4)));
static uint8_t line_buf_fill_idx = 0;
static uint8_t line_buf_dma_idx = 0;
static uint32_t v_scanline = 2;
static bool vactive_cmdlist_posted = false;
static bool dma_pong = false;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;
static video_output_vsync_cb_t vsync_callback = NULL;

static volatile bool resync_requested = false;
static volatile int resync_count = 0;
#define DMACH_PING 0
#define DMACH_PONG 1

static uint32_t vblank_line_vsync_off[] = {HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
                                           SYNC_V1_H1,
                                           HSTX_CMD_NOP,
                                           HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
                                           SYNC_V1_H0,
                                           HSTX_CMD_NOP,
                                           HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
                                           SYNC_V1_H1,
                                           HSTX_CMD_NOP};

static uint32_t vblank_line_vsync_on[] = {HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
                                          SYNC_V0_H1,
                                          HSTX_CMD_NOP,
                                          HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
                                          SYNC_V0_H0,
                                          HSTX_CMD_NOP,
                                          HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
                                          SYNC_V0_H1,
                                          HSTX_CMD_NOP};

static uint32_t vactive_line_dvi[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,  SYNC_V1_H1, HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS};

static uint32_t vactive_di_ping[128], vactive_di_pong[128], vactive_di_null[128];
static uint32_t vactive_di_len, vactive_di_null_len;

static uint32_t vblank_di_ping[128], vblank_di_pong[128], vblank_di_null[128];
static uint32_t vblank_di_len, vblank_di_null_len;

static uint32_t vblank_di_null_vsync_on[128];
static uint32_t vblank_di_null_vsync_on_len;

static uint32_t vblank_acr_vsync_on[64], vblank_acr_vsync_on_len;
static uint32_t vblank_acr_vsync_off[64], vblank_acr_vsync_off_len;
static uint32_t vblank_infoframe_vsync_on[64], vblank_infoframe_vsync_on_len;
static uint32_t vblank_infoframe_vsync_off[64], vblank_infoframe_vsync_off_len;
static uint32_t vblank_avi_infoframe[64], vblank_avi_infoframe_len;

static void __not_in_flash_func(hstx_resync)(void)
{

    hw_clear_bits(&dma_hw->ch[DMACH_PING].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[DMACH_PONG].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);

    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);

    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    __asm volatile("nop\nnop\nnop\nnop");

    v_scanline = 0;
    vactive_cmdlist_posted = false;
    dma_pong = false;
    line_buf_fill_idx = 0;
    line_buf_dma_idx = 0;

    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);

    hw_set_bits(&dma_hw->ch[DMACH_PING].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_set_bits(&dma_hw->ch[DMACH_PONG].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);

    dma_channel_hw_t *ch_ping = &dma_hw->ch[DMACH_PING];
    ch_ping->read_addr = (uintptr_t)vblank_line_vsync_off;
    ch_ping->transfer_count = count_of(vblank_line_vsync_off);

    dma_channel_hw_t *ch_pong = &dma_hw->ch[DMACH_PONG];
    ch_pong->read_addr = (uintptr_t)vblank_line_vsync_off;
    ch_pong->transfer_count = count_of(vblank_line_vsync_off);

    hstx_ctrl_hw->csr |= HSTX_CTRL_CSR_EN_BITS;
    dma_channel_start(DMACH_PING);
}

static uint32_t __not_in_flash_func(build_line_with_di)(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active)
{
    uint32_t *p = buf;
    uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    uint32_t preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;

    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++)
        *p++ = di_words[i];
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | SYNC_AFTER_DI;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_NOP;

    if (active) {

        uint32_t video_preamble = vsync ? VIDEO_PREAMBLE_V0_H1 : VIDEO_PREAMBLE_V1_H1;

        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH - W_VIDEO_PREAMBLE - W_VIDEO_GUARD_BAND);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_PREAMBLE;
        *p++ = video_preamble;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;

        *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }
    return (uint32_t)(p - buf);
}

typedef struct {
    bool vsync_active;
    bool front_porch;
    bool back_porch;
    bool active_video;
    bool send_acr;
    uint32_t active_line;
} scanline_state_t;

static inline void __not_in_flash_func(get_scanline_state)(uint32_t v_scanline, scanline_state_t *state)
{
    state->vsync_active = (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH));
    state->front_porch = (v_scanline < MODE_V_FRONT_PORCH);
    state->back_porch = (v_scanline >= MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH &&
                         v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH);
    state->active_video = (!state->vsync_active && !state->front_porch && !state->back_porch);

    state->send_acr = (v_scanline >= (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) &&
                       v_scanline < (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES) && (v_scanline % 4 == 0));

    if (state->active_video) {
        state->active_line = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    } else {
        state->active_line = 0;
    }
}

static inline void __not_in_flash_func(video_output_handle_vsync)(dma_channel_hw_t *ch, uint32_t v_scanline)
{
    if (dvi_mode) {

#if PICO_HDMI_DVI_USE_DATA_ISLANDS
        ch->read_addr = (uintptr_t)vblank_di_null_vsync_on;
        ch->transfer_count = vblank_di_null_vsync_on_len;
#else
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
#endif
        if (v_scanline == MODE_V_FRONT_PORCH) {
            video_frame_count++;
            if (vsync_callback)
                vsync_callback();
        }
    } else {
        if (v_scanline == MODE_V_FRONT_PORCH) {
            ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
            ch->transfer_count = vblank_acr_vsync_on_len;
            video_frame_count++;
            if (vsync_callback)
                vsync_callback();
        } else {
            ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
            ch->transfer_count = vblank_infoframe_vsync_on_len;
        }
    }
}

static inline void __not_in_flash_func(video_output_handle_active_start)(dma_channel_hw_t *ch, uint32_t v_scanline, uint32_t active_line, bool dma_pong)
{

    if (dvi_mode) {
#if PICO_HDMI_DVI_USE_DATA_ISLANDS
        ch->read_addr = (uintptr_t)vactive_di_null;
        ch->transfer_count = vactive_di_null_len;
#else
        ch->read_addr = (uintptr_t)vactive_line_dvi;
        ch->transfer_count = count_of(vactive_line_dvi);
#endif
    } else {
        uint32_t *buf = dma_pong ? vactive_di_ping : vactive_di_pong;
        const uint32_t *di_words = hstx_di_queue_get_audio_packet();
        if (di_words) {
            vactive_di_len = build_line_with_di(buf, di_words, false, true);
            ch->read_addr = (uintptr_t)buf;
            ch->transfer_count = vactive_di_len;
        } else {
            ch->read_addr = (uintptr_t)vactive_di_null;
            ch->transfer_count = vactive_di_null_len;
        }
    }

    uint8_t fill_idx = line_buf_fill_idx;
    line_buf_dma_idx = fill_idx;
    line_buf_fill_idx = fill_idx ^ 1;

    uint32_t *dst32 = (uint32_t *)line_buffer[fill_idx];
    if (scanline_callback) {
        scanline_callback(v_scanline, active_line, dst32);
    } else {
        for (uint32_t i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++) {
            dst32[i] = 0;
        }
    }
}

static inline void __not_in_flash_func(video_output_handle_blanking)(dma_channel_hw_t *ch, uint32_t v_scanline, bool send_acr, bool dma_pong)
{
    if (dvi_mode) {
        (void)send_acr;
        (void)dma_pong;
        (void)v_scanline;
#if PICO_HDMI_DVI_USE_DATA_ISLANDS
        ch->read_addr = (uintptr_t)vblank_di_null;
        ch->transfer_count = vblank_di_null_len;
#else
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
#endif
    } else {
        if (send_acr) {
            ch->read_addr = (uintptr_t)vblank_acr_vsync_off;
            ch->transfer_count = vblank_acr_vsync_off_len;
        } else if (v_scanline == 0) {
            ch->read_addr = (uintptr_t)vblank_avi_infoframe;
            ch->transfer_count = vblank_avi_infoframe_len;
        } else {
            const uint32_t *di_words = hstx_di_queue_get_audio_packet();
            if (di_words) {
                uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
                vblank_di_len = build_line_with_di(buf, di_words, false, false);
                ch->read_addr = (uintptr_t)buf;
                ch->transfer_count = vblank_di_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_di_null;
                ch->transfer_count = vblank_di_null_len;
            }
        }
    }
}

static inline void __not_in_flash_func(video_output_handle_active_data)(dma_channel_hw_t *ch)
{
    ch->read_addr = (uintptr_t)line_buffer[line_buf_dma_idx];
    ch->transfer_count = (MODE_H_ACTIVE_PIXELS * sizeof(uint16_t)) / sizeof(uint32_t);
}

void __not_in_flash_func(dma_irq_handler)(void)
{
    #if HSTX_DEBUG
    irq_count++;
    #endif
    uint32_t ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1U << ch_num;
    dma_pong = !dma_pong;

    if (!dvi_mode && !vactive_cmdlist_posted) {
        hstx_di_queue_tick();
    }

    scanline_state_t state;
    get_scanline_state(v_scanline, &state);

    if (state.vsync_active) {
        video_output_handle_vsync(ch, v_scanline);
    } else if (state.active_video && !vactive_cmdlist_posted) {
        video_output_handle_active_start(ch, v_scanline, state.active_line, dma_pong);
        vactive_cmdlist_posted = true;
    } else if (state.active_video && vactive_cmdlist_posted) {
        video_output_handle_active_data(ch);
        vactive_cmdlist_posted = false;
    } else {
        video_output_handle_blanking(ch, v_scanline, state.send_acr, dma_pong);
    }
    if (!vactive_cmdlist_posted)
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
}

static void get_acr_params(uint32_t sample_rate, uint32_t *n, uint32_t *cts)
{
    switch (sample_rate) {
        case 32000:
            *n = 4096;
            *cts = 25200;
            break;
        case 44100:
            *n = 6272;
            *cts = 28000;
            break;
        case 48000:
            *n = 6144;
            *cts = 25200;
            break;
        case 88200:
            *n = 12544;
            *cts = 28000;
            break;
        case 96000:
            *n = 12288;
            *cts = 25200;
            break;
        case 176400:
            *n = 25088;
            *cts = 28000;
            break;
        case 192000:
            *n = 24576;
            *cts = 25200;
            break;
        default:
            *n = 6144;
            *cts = 25200;
            break;
    }
}

static uint32_t configured_audio_sample_rate = 48000;

uint32_t pico_hdmi_get_audio_sample_rate(void)
{
    return configured_audio_sample_rate;
}

static void configure_audio_packets(uint32_t sample_rate)
{
    configured_audio_sample_rate = sample_rate;
    hstx_di_queue_set_sample_rate(sample_rate);

    hstx_packet_set_cs_sample_rate(sample_rate);

    hstx_packet_t packet;
    hstx_data_island_t island;

    uint32_t acr_n;
    uint32_t acr_cts;
    get_acr_params(sample_rate, &acr_n, &acr_cts);
    hstx_packet_set_acr(&packet, acr_n, acr_cts);
    hstx_encode_data_island(&island, &packet, true, true);
    vblank_acr_vsync_on_len = build_line_with_di(vblank_acr_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_acr_vsync_off_len = build_line_with_di(vblank_acr_vsync_off, island.words, false, false);

    hstx_packet_set_audio_infoframe(&packet, sample_rate, 2, 16);
    hstx_encode_data_island(&island, &packet, true, true);
    vblank_infoframe_vsync_on_len = build_line_with_di(vblank_infoframe_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_infoframe_vsync_off_len = build_line_with_di(vblank_infoframe_vsync_off, island.words, false, false);
}

void video_output_init(uint16_t width, uint16_t height)
{
    frame_width = width;
    frame_height = height;

#ifndef NOHSTXCLOCK
    uint32_t sys_freq = clock_get_hz(clk_sys);

#ifdef JACK_HSTX_126MHZ

    clock_configure_int_divider(clk_hstx,
                                0,
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq, 2);
#else
    clock_configure_int_divider(clk_hstx,
                                0,
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq, MODE_HSTX_CLK_DIV);
#endif
#endif

    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    configure_audio_packets(48000);

    hstx_packet_t packet;
    hstx_data_island_t island;

#ifdef VIDEO_MODE_320x240

    hstx_packet_set_avi_infoframe(&packet, 1, 3);
#else
    hstx_packet_set_avi_infoframe(&packet, 1, 0);
#endif
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_avi_infoframe_len = build_line_with_di(vblank_avi_infoframe, island.words, false, false);

    vblank_di_null_len = build_line_with_di(vblank_di_null, hstx_get_null_data_island(false, true), false, false);
    vactive_di_null_len = build_line_with_di(vactive_di_null, hstx_get_null_data_island(false, true), false, true);

    vblank_di_null_vsync_on_len = build_line_with_di(vblank_di_null_vsync_on, hstx_get_null_data_island(true, true), true, false);

    vblank_di_len = build_line_with_di(vblank_di_ping, hstx_get_null_data_island(false, true), false, false);
    memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));
}

void video_output_stop(void)
{

    irq_set_enabled(DMA_IRQ_0, false);

    hw_clear_bits(&dma_hw->ch[DMACH_PING].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[DMACH_PONG].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);
    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    dma_hw->inte0 &= ~((1U << DMACH_PING) | (1U << DMACH_PONG));

    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    irq_remove_handler(DMA_IRQ_0, dma_irq_handler);
    dma_channel_unclaim(DMACH_PING);
    dma_channel_unclaim(DMACH_PONG);

    resync_requested = false;
}

void video_output_set_background_task(video_output_task_fn task)
{
    background_task = task;
}

void video_output_request_resync(void)
{
    resync_requested = true;
}

bool video_output_get_dvi_mode(void)
{
    return dvi_mode;
}

void video_output_set_dvi_mode(bool enabled)
{
    dvi_mode = enabled;
}

void video_output_set_scanline_callback(video_output_scanline_cb_t cb)
{
    scanline_callback = cb;
}

void video_output_set_vsync_callback(video_output_vsync_cb_t cb)
{
    vsync_callback = cb;
}

void __not_in_flash_func(video_output_core1_run)(void)
{
#if 0

    hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                                5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                                4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 13 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
#else

    hstx_ctrl_hw->expand_tmds =
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB |
        4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        2 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        4 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        7 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB;

    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

#endif
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS | (uint32_t)MODE_HSTX_CSR_CLKDIV << HSTX_CTRL_CSR_CLKDIV_LSB |
                        5U << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2U << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

    int clk_bit_p = HSTX_BIT_FROM_GPIO(GPIOHSTXCK);
    int clk_bit_n = GPIOHSTXINVERTED ? (clk_bit_p - 1) : (clk_bit_p + 1);
    hstx_ctrl_hw->bit[clk_bit_p] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[clk_bit_n] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

    for (uint lane = 0; lane < 3; ++lane) {

        static const int lane_to_output_bit[3] = {
            HSTX_BIT_FROM_GPIO(GPIOHSTXD0),
            HSTX_BIT_FROM_GPIO(GPIOHSTXD1),
            HSTX_BIT_FROM_GPIO(GPIOHSTXD2)};
        int bit = lane_to_output_bit[lane];
        int bit_dn = GPIOHSTXINVERTED ? (bit - 1) : (bit + 1);

        uint32_t lane_data_sel_bits =
            (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;

        hstx_ctrl_hw->bit[bit] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit_dn] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
    }

    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, count_of(vblank_line_vsync_off),
                          false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, count_of(vblank_line_vsync_off),
                          false);

    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    dma_hw->inte0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);

    #define VIDEO_OUTPUT_WATCHDOG_US 500000u

    #define VIDEO_OUTPUT_OVERRATE_FPS 75u
    uint32_t last_frame_us = time_us_32();
    uint32_t last_frame_count_seen = video_frame_count;
    uint32_t overrate_last_us = last_frame_us;
    uint32_t overrate_last_frames = last_frame_count_seen;
#if HSTX_DEBUG

    uint32_t rate_last_us = last_frame_us;
    uint32_t rate_last_frames = last_frame_count_seen;
    uint32_t rate_last_irqs = irq_count;
#endif
    while (1) {

        uint32_t now = time_us_32();
        uint32_t current_count = video_frame_count;

        if (current_count != last_frame_count_seen)
        {
            last_frame_count_seen = current_count;
            last_frame_us = now;
        }

#if HSTX_DEBUG

        if ((now - rate_last_us) >= 1000000u)
        {
            uint32_t d_us = now - rate_last_us;
            uint32_t d_frames = current_count - rate_last_frames;
            uint32_t d_irqs = irq_count - rate_last_irqs;
            printf("video rate: %lu frames/s, %lu irqs/s (dvi=%d) clk_sys=%lu clk_hstx=%lu csr=%08lx resync=%d di_lvl=%lu underrun=%lu (+%lu/s)\n",
                   (unsigned long)((uint64_t)d_frames * 1000000u / d_us),
                   (unsigned long)((uint64_t)d_irqs * 1000000u / d_us),
                   dvi_mode ? 1 : 0,
                   (unsigned long)clock_get_hz(clk_sys),
                   (unsigned long)clock_get_hz(clk_hstx),
                   (unsigned long)hstx_ctrl_hw->csr,
                   resync_count,
                   (unsigned long)hstx_di_queue_get_level(),
                   (unsigned long)hstx_di_queue_get_underrun_count(),
                   (unsigned long)hstx_di_queue_get_underrun_delta());
            printf("  hstx_div=%08lx exp_sh=%08lx exp_tmds=%08lx ping_ctrl=%08lx pong_ctrl=%08lx\n",
                   (unsigned long)clocks_hw->clk[clk_hstx].div,
                   (unsigned long)hstx_ctrl_hw->expand_shift,
                   (unsigned long)hstx_ctrl_hw->expand_tmds,
                   (unsigned long)dma_hw->ch[DMACH_PING].al1_ctrl,
                   (unsigned long)dma_hw->ch[DMACH_PONG].al1_ctrl);
            uint32_t fc_sys_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
            uint32_t fc_hstx_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX);
            printf("  sys_ctrl=%08lx sys_div=%08lx pll_fbdiv=%lu pll_prim=%08lx fc_sys=%lu kHz fc_hstx=%lu kHz\n",
                   (unsigned long)clocks_hw->clk[clk_sys].ctrl,
                   (unsigned long)clocks_hw->clk[clk_sys].div,
                   (unsigned long)pll_sys_hw->fbdiv_int,
                   (unsigned long)pll_sys_hw->prim,
                   (unsigned long)fc_sys_khz,
                   (unsigned long)fc_hstx_khz);
            rate_last_us = now;
            rate_last_frames = current_count;
            rate_last_irqs = irq_count;
        }
#endif

        if ((now - overrate_last_us) >= 1000000u)
        {
            uint32_t d_us = now - overrate_last_us;
            uint32_t d_frames = current_count - overrate_last_frames;
            uint32_t fps = (uint32_t)((uint64_t)d_frames * 1000000u / d_us);
            if (fps > VIDEO_OUTPUT_OVERRATE_FPS)
            {
                resync_requested = true;
            }
            overrate_last_us = now;
            overrate_last_frames = current_count;
        }

        bool stall_wd = (now - last_frame_us) > VIDEO_OUTPUT_WATCHDOG_US;
        bool do_resync = resync_requested || stall_wd;

        if (do_resync)
        {

            const char *cause = (resync_requested && stall_wd) ? "overrate+stall"
                                : (resync_requested ? "overrate" : "stall");
            resync_count++;
            irq_set_enabled(DMA_IRQ_0, false);
            hstx_resync();
            resync_requested = false;
            irq_set_enabled(DMA_IRQ_0, true);
            last_frame_us = time_us_32();
            last_frame_count_seen = video_frame_count;
            overrate_last_us = last_frame_us;
            overrate_last_frames = video_frame_count;
            printf("HSTX resync performed! cause=%s di_level=%lu Total resyncs since boot: %d\n",
                   cause, (unsigned long)hstx_di_queue_get_level(), resync_count);
        }

        if (background_task) {
            background_task();
        }
        tight_loop_contents();
    }
}

void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate)
{
    configure_audio_packets(sample_rate);
}

int get_video_output_resync_count(void)
{
    return resync_count;
}
