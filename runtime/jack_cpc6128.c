// SPDX-License-Identifier: GPL-2.0-or-later
#include "jack_cpc6128.h"

#include <string.h>
#ifdef PICO_ON_DEVICE
#include "pico/time.h"
#endif

#include "asic.h"
#include "cap32.h"
#include "z80.h"

#define CPC_CRTC_TICKS_PER_LINE 64u
#define CPC_LINES_PER_FRAME 312u
#define CPC_CRTC_TICKS_PER_FRAME (CPC_CRTC_TICKS_PER_LINE * CPC_LINES_PER_FRAME)
#define CPC_AUDIO_FRAMES_PER_VIDEO_FRAME (JACK_CPC_AUDIO_RATE / 50u)

extern t_z80regs z80;

t_CPC CPC;
t_CRTC CRTC;
t_FDC FDC;
t_GateArray GateArray;
t_PPI PPI;
t_PSG PSG;
t_VDU VDU;
t_asic asic;

uint8_t *pbTapeImage;
uint8_t *pbRegisterPage;
uint32_t dwMF2Flags;
uint32_t dwMF2ExitAddr;
int iTapeCycleCount;
uint8_t *membank_read[4];
uint8_t *membank_write[4];

static jack_cpc6128 *active;
static uint8_t *bank_config[8][4];

typedef struct {
    uint8_t command[9];
    uint8_t command_length;
    uint8_t command_position;
    uint8_t result[7];
    uint8_t result_length;
    uint8_t result_position;
    const uint8_t *sector_data;
    size_t sector_offset;
    size_t sector_size;
    size_t sector_position;
    uint8_t cylinder;
    uint8_t head;
    uint8_t sector;
    uint8_t size_code;
    uint8_t end_sector;
    uint8_t status0;
} jack_cpc_fdc_state;

static jack_cpc_fdc_state fdc_state;

static const uint16_t cpc_ink_rgb555[32] = {
    0x3def,0x3def,0x03ef,0x7fef,0x0010,0x7c10,0x0210,0x3e10,
    0x7c10,0x7fef,0x7fe0,0x7fff,0x7c00,0x7c1f,0x7e00,0x7e1f,
    0x0010,0x03ef,0x03e0,0x03ff,0x0000,0x001f,0x0200,0x021f,
    0x4010,0x43ef,0x43e0,0x43ff,0x4000,0x401f,0x4200,0x421f
};

static void map_memory(void) {
    if(active == NULL) return;
    uint8_t *base = active->config.ram;
    uint8_t *ext = base + 0x10000u;
    uint8_t *b0 = base;
    uint8_t *b1 = base + 0x4000u;
    uint8_t *b2 = base + 0x8000u;
    uint8_t *b3 = base + 0xc000u;
    uint8_t *b4 = ext;
    uint8_t *b5 = ext + 0x4000u;
    uint8_t *b6 = ext + 0x8000u;
    uint8_t *b7 = ext + 0xc000u;
    uint8_t *maps[8][4] = {
        {b0,b1,b2,b3},{b0,b1,b2,b7},{b4,b5,b6,b7},{b0,b3,b2,b7},
        {b0,b4,b2,b3},{b0,b5,b2,b3},{b0,b6,b2,b3},{b0,b7,b2,b3}
    };
    memcpy(bank_config, maps, sizeof(bank_config));
    for(unsigned index = 0; index < 4u; ++index) {
        membank_read[index] = bank_config[GateArray.RAM_config & 7u][index];
        membank_write[index] = bank_config[GateArray.RAM_config & 7u][index];
    }
    if((GateArray.ROM_config & 0x04u) == 0u && active->rom_loaded[JACK_CPC_ROM_LOWER]) {
        membank_read[0] = active->config.lower_rom;
    }
    if((GateArray.ROM_config & 0x08u) == 0u) {
        uint8_t *upper = active->config.basic_rom;
        if(active->selected_upper_rom == 7u && active->rom_loaded[JACK_CPC_ROM_AMSDOS]) {
            upper = active->config.amsdos_rom;
        }
        if(active->rom_loaded[JACK_CPC_ROM_BASIC] ||
           (active->selected_upper_rom == 7u && active->rom_loaded[JACK_CPC_ROM_AMSDOS])) {
            membank_read[3] = upper;
        }
    }
}

static bool dsk_track(const jack_cpc6128 *machine, uint8_t cylinder, uint8_t head,
                      const uint8_t **track, size_t *track_size) {
    if(!machine->dsk_mounted || cylinder >= machine->dsk_tracks || head >= machine->dsk_sides) {
        return false;
    }
    const size_t wanted = (size_t)cylinder * machine->dsk_sides + head;
    size_t offset = 0x100u;
    if(machine->dsk_extended) {
        for(size_t index = 0; index < wanted; ++index) {
            offset += (size_t)machine->dsk_data[0x34u + index] << 8u;
        }
        *track_size = (size_t)machine->dsk_data[0x34u + wanted] << 8u;
    } else {
        const size_t fixed = machine->dsk_data[0x32] | ((size_t)machine->dsk_data[0x33] << 8u);
        offset += wanted * fixed;
        *track_size = fixed;
    }
    if(*track_size < 0x100u || offset > machine->dsk_size || *track_size > machine->dsk_size - offset) {
        return false;
    }
    *track = machine->dsk_data + offset;
    return true;
}

static bool dsk_sector(const jack_cpc6128 *machine, uint8_t cylinder, uint8_t head,
                       uint8_t sector_id, uint8_t size_code, const uint8_t **data,
                       size_t *size) {
    const uint8_t *track;
    size_t track_size;
    if(!dsk_track(machine, cylinder, head, &track, &track_size) ||
       memcmp(track, "Track-Info\r\n", 12u) != 0) return false;
    const uint8_t count = track[0x15];
    if(count > DSK_SECTORMAX || 0x18u + (size_t)count * 8u > 0x100u) return false;
    size_t data_offset = 0x100u;
    for(uint8_t index = 0; index < count; ++index) {
        const uint8_t *descriptor = track + 0x18u + (size_t)index * 8u;
        const uint8_t n = descriptor[3];
        size_t bytes = 128u << (n < 7u ? n : 7u);
        if(machine->dsk_extended) {
            const size_t explicit_size = descriptor[6] | ((size_t)descriptor[7] << 8u);
            if(explicit_size != 0u) bytes = explicit_size;
        }
        if(data_offset > track_size || bytes > track_size - data_offset) return false;
        if(descriptor[0] == cylinder && descriptor[1] == head && descriptor[2] == sector_id &&
           (descriptor[3] == size_code || size_code == 0xffu)) {
            *data = track + data_offset;
            *size = bytes;
            return true;
        }
        data_offset += bytes;
    }
    return false;
}

static void fdc_set_result(uint8_t st0, uint8_t st1, uint8_t st2, uint8_t c,
                           uint8_t h, uint8_t r, uint8_t n) {
    const uint8_t values[7] = {st0,st1,st2,c,h,r,n};
    memcpy(fdc_state.result, values, sizeof(values));
    fdc_state.result_length = 7u;
    fdc_state.result_position = 0u;
    FDC.phase = RESULT_PHASE;
}

static uint8_t fdc_command_length(uint8_t command) {
    switch(command & 0x1fu) {
        case 0x03: return 3u;
        case 0x04: return 2u;
        case 0x05: return 9u;
        case 0x06: return 9u;
        case 0x07: return 2u;
        case 0x08: return 1u;
        case 0x0a: return 2u;
        case 0x0f: return 3u;
        default: return 1u;
    }
}

static void fdc_begin_read_sector(void) {
    if(active != NULL && FDC.motor && dsk_sector(active, fdc_state.cylinder,
       fdc_state.head, fdc_state.sector, fdc_state.size_code,
       &fdc_state.sector_data, &fdc_state.sector_size)) {
        fdc_state.sector_offset = (size_t)(fdc_state.sector_data - active->dsk_data);
        fdc_state.sector_position = 0u;
        FDC.phase = EXEC_PHASE;
        FDC.timeout = 0x3fffffff;
        FDC.cmd_direction = FDC_TO_CPU;
    } else {
        fdc_set_result((uint8_t)(0x40u | fdc_state.head << 2u), 0x04u, 0u,
                       fdc_state.cylinder, fdc_state.head, fdc_state.sector,
                       fdc_state.size_code);
    }
}

static void fdc_begin_write_sector(void) {
    if(active != NULL && active->dsk_overlay != NULL && FDC.motor &&
       dsk_sector(active, fdc_state.cylinder, fdc_state.head, fdc_state.sector,
                  fdc_state.size_code, &fdc_state.sector_data, &fdc_state.sector_size)) {
        fdc_state.sector_offset = (size_t)(fdc_state.sector_data - active->dsk_data);
        fdc_state.sector_position = 0u;
        FDC.phase = EXEC_PHASE;
        FDC.timeout = 0x3fffffff;
        FDC.cmd_direction = CPU_TO_FDC;
    } else {
        fdc_set_result((uint8_t)(0x40u | fdc_state.head << 2u), 0x02u, 0u,
                       fdc_state.cylinder, fdc_state.head, fdc_state.sector,
                       fdc_state.size_code);
    }
}

static void fdc_execute_command(void) {
    const uint8_t command = fdc_state.command[0] & 0x1fu;
    switch(command) {
        case 0x03:
            FDC.phase = CMD_PHASE;
            break;
        case 0x04:
            fdc_state.result[0] = (uint8_t)((FDC.motor ? 0x20u : 0u) |
                (active != NULL && active->dsk_mounted ? 0u : 0x08u) |
                (fdc_state.cylinder == 0u ? 0x10u : 0u));
            fdc_state.result_length = 1u;
            fdc_state.result_position = 0u;
            FDC.phase = RESULT_PHASE;
            break;
        case 0x07:
            fdc_state.cylinder = 0u;
            fdc_state.status0 = 0x20u;
            FDC.phase = CMD_PHASE;
            break;
        case 0x08:
            fdc_state.result[0] = fdc_state.status0;
            fdc_state.result[1] = fdc_state.cylinder;
            fdc_state.result_length = 2u;
            fdc_state.result_position = 0u;
            fdc_state.status0 = 0x80u;
            FDC.phase = RESULT_PHASE;
            break;
        case 0x0f:
            fdc_state.head = (fdc_state.command[1] >> 2u) & 1u;
            fdc_state.cylinder = fdc_state.command[2];
            fdc_state.status0 = (uint8_t)(0x20u | fdc_state.head << 2u);
            FDC.phase = CMD_PHASE;
            break;
        case 0x0a: {
            const uint8_t *track;
            size_t track_size;
            fdc_state.head = (fdc_state.command[1] >> 2u) & 1u;
            if(active != NULL && FDC.motor &&
               dsk_track(active, fdc_state.cylinder, fdc_state.head, &track, &track_size) &&
               track_size >= 0x20u && track[0x15] != 0u) {
                const uint8_t *descriptor = track + 0x18u;
                fdc_set_result((uint8_t)(fdc_state.head << 2u), 0u, 0u,
                    descriptor[0],descriptor[1],descriptor[2],descriptor[3]);
            } else {
                fdc_set_result(0x40u,0x04u,0u,fdc_state.cylinder,fdc_state.head,0u,0u);
            }
            break;
        }
        case 0x05:
            fdc_state.cylinder = fdc_state.command[2];
            fdc_state.head = fdc_state.command[3];
            fdc_state.sector = fdc_state.command[4];
            fdc_state.size_code = fdc_state.command[5];
            fdc_state.end_sector = fdc_state.command[6];
            fdc_begin_write_sector();
            break;
        case 0x06:
            fdc_state.cylinder = fdc_state.command[2];
            fdc_state.head = fdc_state.command[3];
            fdc_state.sector = fdc_state.command[4];
            fdc_state.size_code = fdc_state.command[5];
            fdc_state.end_sector = fdc_state.command[6];
            fdc_begin_read_sector();
            break;
        default:
            fdc_state.result[0] = 0x80u;
            fdc_state.result_length = 1u;
            fdc_state.result_position = 0u;
            FDC.phase = RESULT_PHASE;
            break;
    }
    fdc_state.command_position = 0u;
    fdc_state.command_length = 0u;
}

uint8_t fdc_read_status(void) {
    if(FDC.phase == EXEC_PHASE)
        return FDC.cmd_direction==FDC_TO_CPU?0xf0u:0xb0u;
    if(FDC.phase == RESULT_PHASE) return 0xd0u;
    return fdc_state.command_position?0x90u:0x80u;
}

uint8_t fdc_read_data(void) {
    if(FDC.phase == EXEC_PHASE && fdc_state.sector_data != NULL) {
        uint8_t value = fdc_state.sector_data[fdc_state.sector_position];
        if(active->dsk_overlay != NULL) {
            (void)jack_cpc_dsk_overlay_read(active->dsk_overlay,
                fdc_state.sector_offset + fdc_state.sector_position, &value);
        }
        fdc_state.sector_position++;
        FDC.timeout = 0x3fffffff;
        if(fdc_state.sector_position >= fdc_state.sector_size) {
            if(fdc_state.sector < fdc_state.end_sector) {
                ++fdc_state.sector;
                fdc_begin_read_sector();
            } else {
                fdc_set_result((uint8_t)(fdc_state.head << 2u),0u,0u,fdc_state.cylinder,
                    fdc_state.head,fdc_state.sector,fdc_state.size_code);
            }
        }
        return value;
    }
    if(FDC.phase == RESULT_PHASE && fdc_state.result_position < fdc_state.result_length) {
        const uint8_t value = fdc_state.result[fdc_state.result_position++];
        if(fdc_state.result_position >= fdc_state.result_length) FDC.phase = CMD_PHASE;
        return value;
    }
    return 0xffu;
}

void fdc_write_data(unsigned char value) {
    if(FDC.phase == EXEC_PHASE && FDC.cmd_direction == CPU_TO_FDC &&
       active != NULL && active->dsk_overlay != NULL) {
        if(!jack_cpc_dsk_overlay_write(active->dsk_overlay,
           fdc_state.sector_offset + fdc_state.sector_position, value)) {
            fdc_set_result((uint8_t)(0x40u | fdc_state.head << 2u), 0x02u, 0u,
                           fdc_state.cylinder, fdc_state.head, fdc_state.sector,
                           fdc_state.size_code);
            return;
        }
        fdc_state.sector_position++;
        FDC.timeout = 0x3fffffff;
        if(fdc_state.sector_position >= fdc_state.sector_size) {
            if(fdc_state.sector < fdc_state.end_sector) {
                ++fdc_state.sector;
                fdc_begin_write_sector();
            } else {
                fdc_set_result((uint8_t)(fdc_state.head << 2u), 0u, 0u,
                               fdc_state.cylinder, fdc_state.head, fdc_state.sector,
                               fdc_state.size_code);
            }
        }
        return;
    }
    if(FDC.phase != CMD_PHASE) return;
    if(fdc_state.command_position == 0u) {
        fdc_state.command_length = fdc_command_length(value);
    }
    if(fdc_state.command_position < sizeof(fdc_state.command)) {
        fdc_state.command[fdc_state.command_position++] = value;
    }
    if(fdc_state.command_position >= fdc_state.command_length) fdc_execute_command();
}

static void ay_bus_write(uint8_t data) {
    const uint8_t control = PSG.control & 0xc0u;
    if(control == 0xc0u) PSG.reg_select = data & 0x0fu;
else if(control == 0x80u && PSG.reg_select < 16u) {
        PSG.RegisterAY.Index[PSG.reg_select] = data;
        if(active != NULL) jack_ay8912_register_written(&active->ay, PSG.reg_select, data);
    }
}

static uint8_t ppi_read(uint8_t port) {
    switch(port & 3u) {
        case 0:
            if((PPI.control & 0x10u) != 0u && (PSG.control & 0xc0u) == 0x40u &&
               PSG.reg_select < 16u) {
                if(PSG.reg_select == 14u) return active->keyboard_matrix[CPC.keyboard_line & 0x0fu];
                return PSG.RegisterAY.Index[PSG.reg_select];
            }
            return PPI.portA;
        case 1:
            return (uint8_t)(0x40u | (CRTC.flag_invsync ? 1u : 0u));
        case 2:
            return PPI.portC;
        default:
            return 0xffu;
    }
}

static void ppi_apply_port_c(void) {
    if((PPI.control & 1u) == 0u) CPC.keyboard_line = PPI.portC & 0x0fu;
    if((PPI.control & 8u) == 0u) {
        CPC.tape_motor = PPI.portC & 0x10u;
        PSG.control = PPI.portC;
        ay_bus_write(PPI.portA);
    }
}

static void ppi_write(uint8_t port, uint8_t value) {
    switch(port & 3u) {
        case 0:
            PPI.portA = value;
            if((PPI.control & 0x10u) == 0u) ay_bus_write(value);
            break;
        case 1:
            PPI.portB = value;
            break;
        case 2:
            PPI.portC = value;
            ppi_apply_port_c();
            break;
        case 3:
            if((value & 0x80u) != 0u) {
                PPI.control = value;
                PPI.portA = PPI.portB = PPI.portC = 0u;
                ppi_apply_port_c();
            } else {
                const uint8_t mask = (uint8_t)(1u << ((value >> 1u) & 7u));
                if((value & 1u) != 0u) PPI.portC |= mask;
                else PPI.portC &= (uint8_t)~mask;
                ppi_apply_port_c();
            }
            break;
    }
}

uint8_t z80_IN_handler(reg_pair port) {
    if(active == NULL) return 0xffu;
    ++active->io_reads;
    const uint8_t high = port.b.h;
    if((high & 0x40u) == 0u) {
        if((high & 3u) == 3u && CRTC.reg_select >= 12u && CRTC.reg_select < 18u) {
            return CRTC.registers[CRTC.reg_select];
        }
        return 0u;
    }
    if((high & 0x08u) == 0u) return ppi_read(high & 3u);
    if((high & 0x04u) == 0u && high == 0xfbu && (port.b.l & 0x80u) == 0u) {
        return (port.b.l & 1u) == 0u ? fdc_read_status() : fdc_read_data();
    }
    return 0xffu;
}

void z80_OUT_handler(reg_pair port, uint8_t value) {
    if(active == NULL) return;
    ++active->io_writes;
    const uint8_t high = port.b.h;
    if((high & 0xc0u) == 0x40u) {
        switch(value >> 6u) {
            case 0:
                GateArray.pen = (value & 0x10u) != 0u ? 16u : value & 0x0fu;
                break;
            case 1:
                GateArray.ink_values[GateArray.pen] = value & 0x1fu;
                break;
            case 2:
                GateArray.ROM_config = value;
                GateArray.requested_scr_mode = value & 3u;
                active->current_mode = value & 3u;
                if((value & 0x10u) != 0u) {
                    z80.int_pending = 0u;
                    GateArray.sl_count = 0u;
                }
                map_memory();
                break;
            default:
                break;
        }
    }
    if((high & 0x80u) == 0u && (value & 0xc0u) == 0xc0u) {
        GateArray.RAM_config = value;
        map_memory();
    }
    if((high & 0x40u) == 0u) {
        const uint8_t crtc_port = high & 3u;
        if(crtc_port == 0u) CRTC.reg_select = value & 0x1fu;
        else if(crtc_port == 1u && CRTC.reg_select < 16u) {
            CRTC.registers[CRTC.reg_select] = value;
            if(CRTC.reg_select == 3u) {
                CRTC.hsw = value & 0x0fu;
                CRTC.vsw = value >> 4u;
            }
        }
    }
    if((high & 0x20u) == 0u) {
        active->selected_upper_rom = value;
        GateArray.upper_ROM = value;
        map_memory();
    }
    if((high & 0x08u) == 0u) ppi_write(high & 3u, value);
    if(high == 0xfau && (port.b.l & 0x80u) == 0u) FDC.motor = value & 1u;
    else if(high == 0xfbu && (port.b.l & 0x80u) == 0u) fdc_write_data(value);
}

static void render_frame(jack_cpc6128 *machine) {
    if(machine->config.frame_index8 == NULL) return;
    const uint8_t border = 16u;
    for(unsigned y = 0; y < JACK_CPC_FRAME_HEIGHT; ++y) {
        memset(machine->config.frame_index8 + y * machine->config.frame_stride,
               border, JACK_CPC_FRAME_WIDTH);
        machine->frame_source_y[y]=0xffffu;
    }
    const uint16_t start_ma = (uint16_t)((CRTC.registers[12] << 8u) | CRTC.registers[13]);
    uint8_t *screen = machine->config.ram;
    const unsigned columns=CRTC.registers[1]?CRTC.registers[1]:40u;
    const unsigned rows=CRTC.registers[6]?CRTC.registers[6]:25u;
    const unsigned raster_height=CRTC.registers[1]&&CRTC.registers[6]?
        (CRTC.registers[9]&31u)+1u:8u;
    unsigned source_height=rows*raster_height;
    if(source_height>312u) source_height=312u;

    if(columns!=40u||source_height!=200u||raster_height!=8u) {
        static uint16_t source_bytes[320];
        static uint8_t source_pixels[320];
        static uint8_t mode0_pixels[1020];
        const unsigned pixels_per_byte=machine->current_mode==0u?2u:
            (machine->current_mode==2u?8u:4u);
        const unsigned source_width=columns*2u*pixels_per_byte;
        for(unsigned x=0u;x<320u;++x) {
            const unsigned pixel=x*source_width/320u;
            source_bytes[x]=(uint16_t)(pixel/pixels_per_byte);
            source_pixels[x]=(uint8_t)(pixel%pixels_per_byte);
        }
        const unsigned output_height=source_height<240u?source_height:240u;
        const unsigned top=(240u-output_height)/2u;
        for(unsigned y=0u;y<output_height;++y) {
            const unsigned source_y=y*source_height/output_height;
            machine->frame_source_y[top+y]=(uint16_t)source_y;
            const uint16_t ma=(uint16_t)(start_ma+(source_y/raster_height)*columns);
            const uint16_t page=(uint16_t)(((ma&0x3000u)<<2u)|((source_y%raster_height&7u)<<11u));
            const unsigned byte_start=(ma&0x03ffu)*2u;
            uint8_t *dst=machine->config.frame_index8+(top+y)*machine->config.frame_stride;
            if(machine->current_mode==0u) {

                for(unsigned b=0u;b<columns*2u;++b) {
                    const uint8_t v=screen[page|((byte_start+b)&0x07ffu)];
                    mode0_pixels[b*2u]=(uint8_t)(((v>>7u)&1u)|((v>>2u)&2u)|
                        ((v>>3u)&4u)|((v<<2u)&8u));
                    mode0_pixels[b*2u+1u]=(uint8_t)(((v>>6u)&1u)|((v>>1u)&2u)|
                        ((v>>2u)&4u)|((v<<3u)&8u));
                }
                for(unsigned x=0u;x<320u;++x)
                    dst[x]=mode0_pixels[source_bytes[x]*2u+source_pixels[x]];
                continue;
            }
            unsigned cached_byte=0xffffu;
            uint8_t value=0u;
            for(unsigned x=0u;x<320u;++x) {

                if(source_bytes[x]!=cached_byte) {
                    cached_byte=source_bytes[x];
                    value=screen[page|((byte_start+cached_byte)&0x07ffu)];
                }
                const unsigned p=source_pixels[x];
                if(machine->current_mode==0u) {
                    dst[x]=(uint8_t)(((value>>(7u-p))&1u)|((value>>(2u-p))&2u)|
                        ((value>>(3u-p))&4u)|((value<<(2u+p))&8u));
                } else if(machine->current_mode==2u) dst[x]=(value>>(7u-p))&1u;
                else dst[x]=(uint8_t)(((value>>(7u-p))&1u)|(((value>>(3u-p))&1u)<<1u));
            }
        }
        return;
    }
    for(unsigned y = 0; y < 200u; ++y) {
        machine->frame_source_y[y+20u]=(uint16_t)y;
        uint8_t *dst = machine->config.frame_index8 + (y + 20u) * machine->config.frame_stride;
        const uint16_t ma = (uint16_t)(start_ma + (y >> 3u) * 40u);

        const uint16_t row = (uint16_t)(((ma & 0x3000u) << 2u) |
            ((y & 7u) << 11u) | ((ma & 0x03ffu) << 1u));
        if(machine->current_mode == 0u) {
            for(unsigned index = 0; index < 80u; ++index) {
                const uint8_t value = screen[(row&0xf800u)|((row+index)&0x07ffu)];
                const uint8_t p0 = (uint8_t)(((value >> 7u) & 1u) | ((value >> 2u) & 2u) |
                    ((value >> 3u) & 4u) | ((value << 2u) & 8u));
                const uint8_t p1 = (uint8_t)(((value >> 6u) & 1u) | ((value >> 1u) & 2u) |
                    ((value >> 2u) & 4u) | ((value << 3u) & 8u));
                const unsigned x = index * 4u;
                dst[x] = dst[x + 1u] = p0;
                dst[x + 2u] = dst[x + 3u] = p1;
            }
        } else if(machine->current_mode == 2u) {
            for(unsigned index = 0; index < 80u; ++index) {
                const uint8_t value = screen[(row&0xf800u)|((row+index)&0x07ffu)];
                const unsigned x = index * 4u;
                dst[x] = (value >> 7u) & 1u;
                dst[x + 1u] = (value >> 5u) & 1u;
                dst[x + 2u] = (value >> 3u) & 1u;
                dst[x + 3u] = (value >> 1u) & 1u;
            }
        } else {
            for(unsigned index = 0; index < 80u; ++index) {
                const uint8_t value = screen[(row&0xf800u)|((row+index)&0x07ffu)];
                const unsigned x = index * 4u;
                for(unsigned pixel = 0; pixel < 4u; ++pixel) {
                    dst[x + pixel] = (uint8_t)(((value >> (7u - pixel)) & 1u) |
                        (((value >> (3u - pixel)) & 1u) << 1u));
                }
            }
        }
    }
}

static void render_audio(jack_cpc6128 *machine) {
    if(machine->config.audio == NULL) return;
    (void)jack_ay8912_render(&machine->ay, PSG.RegisterAY.Index, machine->config.audio,
                            CPC_AUDIO_FRAMES_PER_VIDEO_FRAME);
}

void crtc_cycle(int repeat_count) {
    if(active == NULL || repeat_count <= 0) return;
    const unsigned vsync_start=CRTC.registers[7]*((CRTC.registers[9]&31u)+1u);
    const unsigned vsync_width=(CRTC.registers[3]>>4u)?(CRTC.registers[3]>>4u):16u;
    while(repeat_count>0) {

        const unsigned phase=active->crtc_ticks&63u;
        const unsigned distance=phase<32u?32u-phase:64u-phase;
        const unsigned step=(unsigned)repeat_count<distance?(unsigned)repeat_count:distance;
        active->crtc_ticks+=step;
        repeat_count-=(int)step;
        const unsigned line=active->crtc_ticks/CPC_CRTC_TICKS_PER_LINE;

        if(line<312u&&active->crtc_ticks%CPC_CRTC_TICKS_PER_LINE==32u)
            memcpy(active->scanline_inks[line],GateArray.ink_values,17u);
        CRTC.flag_invsync=line>=vsync_start&&line<vsync_start+vsync_width;
        if(active->crtc_ticks % CPC_CRTC_TICKS_PER_LINE == 0u) {
            ++GateArray.sl_count;
            if(GateArray.sl_count >= 52u) {
                GateArray.sl_count = 0u;
                z80.int_pending = 1u;
                ++active->interrupt_count;
            }
        }
        if(active->crtc_ticks >= CPC_CRTC_TICKS_PER_FRAME) {
            active->crtc_ticks -= CPC_CRTC_TICKS_PER_FRAME;
            ++active->frame_count;
            VDU.frame_completed = 1u;
#ifdef PICO_ON_DEVICE
            uint32_t started=time_us_32();
#endif
            render_frame(active);
#ifdef PICO_ON_DEVICE
            active->video_us+=time_us_32()-started;
            started=time_us_32();
#endif
            render_audio(active);
#ifdef PICO_ON_DEVICE
            active->audio_us+=time_us_32()-started;
#endif
        }
    }
}

bool asic_register_page_read(uint16_t address, uint8_t *value) {
    (void)address;
    (void)value;
    return true;
}

bool asic_register_page_write(uint16_t address, uint8_t value) {
    (void)address;
    (void)value;
    return true;
}

uint8_t asic_int(void) { return 0xffu; }
void Tape_UpdateLevel(void) {}

bool jack_cpc6128_init(jack_cpc6128 *machine, const jack_cpc6128_config *config) {
    if(machine == NULL || config == NULL || config->ram == NULL ||
       config->ram_size < JACK_CPC_RAM_BYTES || config->lower_rom == NULL ||
       config->basic_rom == NULL || config->amsdos_rom == NULL ||
       config->frame_index8 == NULL || config->frame_stride < JACK_CPC_FRAME_WIDTH) return false;
    memset(machine, 0, sizeof(*machine));
    machine->config = *config;
    jack_ay8912_reset(&machine->ay);
    active = machine;
    z80_init_tables();
    jack_cpc6128_reset(machine);
    return true;
}

bool jack_cpc6128_load_rom(jack_cpc6128 *machine, jack_cpc_rom_kind kind,
                           jack_stream *stream) {
    if(machine == NULL || stream == NULL || kind > JACK_CPC_ROM_AMSDOS ||
       jack_stream_size(stream) != JACK_CPC_ROM_BYTES || !jack_stream_seek(stream, 0u)) return false;
    uint8_t *destination = kind == JACK_CPC_ROM_LOWER ? machine->config.lower_rom :
        (kind == JACK_CPC_ROM_BASIC ? machine->config.basic_rom : machine->config.amsdos_rom);
    if(!jack_stream_read_exact(stream, destination, JACK_CPC_ROM_BYTES)) return false;
    machine->rom_loaded[kind] = true;
    if(active == machine) map_memory();
    return true;
}

bool jack_cpc6128_load_system_rom(jack_cpc6128 *machine, jack_stream *stream) {
    if(machine == NULL || stream == NULL || jack_stream_size(stream) != 2u * JACK_CPC_ROM_BYTES ||
       !jack_stream_seek(stream,0u) ||
       !jack_stream_read_exact(stream,machine->config.lower_rom,JACK_CPC_ROM_BYTES) ||
       !jack_stream_read_exact(stream,machine->config.basic_rom,JACK_CPC_ROM_BYTES)) return false;
    machine->rom_loaded[JACK_CPC_ROM_LOWER] = true;
    machine->rom_loaded[JACK_CPC_ROM_BASIC] = true;
    if(active == machine) map_memory();
    return true;
}

bool jack_cpc6128_mount_dsk(jack_cpc6128 *machine, const void *data, size_t size) {
    if(machine == NULL || data == NULL || size < 0x200u) return false;
    const uint8_t *bytes = (const uint8_t *)data;
    const bool standard = memcmp(bytes, "MV - CPC", 8u) == 0;
    const bool extended = memcmp(bytes, "EXTENDED", 8u) == 0;
    if(!standard && !extended) return false;
    const uint8_t tracks = bytes[0x30];
    const uint8_t sides = bytes[0x31];
    if(tracks == 0u || tracks > DSK_TRACKMAX || sides == 0u || sides > DSK_SIDEMAX ||
       (size_t)tracks * sides > DSK_TRACKMAX * DSK_SIDEMAX) return false;
    machine->dsk_data = bytes;
    machine->dsk_size = size;
    machine->dsk_tracks = tracks;
    machine->dsk_sides = sides;
    machine->dsk_extended = extended;
    machine->dsk_mounted = true;
    machine->dsk_overlay = NULL;
    return true;
}

bool jack_cpc6128_attach_dsk_overlay(jack_cpc6128 *machine,
                                     jack_cpc_dsk_overlay *overlay) {
    if(machine == NULL || overlay == NULL || !machine->dsk_mounted ||
       overlay->base != machine->dsk_data || overlay->base_size != machine->dsk_size) return false;
    machine->dsk_overlay = overlay;
    return true;
}

void jack_cpc6128_eject_dsk(jack_cpc6128 *machine) {
    if(machine == NULL) return;
    machine->dsk_data = NULL;
    machine->dsk_size = 0u;
    machine->dsk_tracks = machine->dsk_sides = 0u;
    machine->dsk_mounted = false;
    machine->dsk_overlay = NULL;
}

void jack_cpc6128_reset(jack_cpc6128 *machine) {
    if(machine == NULL) return;
    active = machine;
    memset(machine->config.ram, 0, JACK_CPC_RAM_BYTES);
    memset(machine->keyboard_matrix, 0xff, sizeof(machine->keyboard_matrix));
    memset(&CPC, 0, sizeof(CPC));
    memset(&CRTC, 0, sizeof(CRTC));
    memset(&FDC, 0, sizeof(FDC));
    memset(&GateArray, 0, sizeof(GateArray));
    memset(&PPI, 0, sizeof(PPI));
    memset(&PSG, 0, sizeof(PSG));
    jack_ay8912_reset(&machine->ay);
    memset(&VDU, 0, sizeof(VDU));
    memset(&asic, 0, sizeof(asic));
    memset(&z80, 0, sizeof(z80));
    memset(&fdc_state, 0, sizeof(fdc_state));
    CPC.model = CPC_MODEL_6128;
    CPC.ram_size = 128u;
    CPC.cycle_count = CYCLE_COUNT_INIT;
    CPC.printer_port = 0xffu;
    GateArray.scr_mode = GateArray.requested_scr_mode = 1u;
    machine->current_mode = 1u;
    GateArray.lower_ROM_bank = 0u;
    PPI.control = 0x9bu;
    FDC.phase = CMD_PHASE;
    FDC.flags = STATUSDRVA_flag | STATUSDRVB_flag;
    asic.locked = true;
    machine->selected_upper_rom = 0u;
    z80.IX.w.l = z80.IY.w.l = 0xffffu;
    z80.SP.w.l = 0xffffu;
    z80.AF.b.l = Zflag;
    z80.break_point = 0xffffffffu;
    map_memory();
    render_frame(machine);
}

int jack_cpc6128_run_frame(jack_cpc6128 *machine) {
    if(machine == NULL || active != machine || !machine->rom_loaded[JACK_CPC_ROM_LOWER]) return -1;
    VDU.frame_completed = 0u;
    CPC.cycle_count = CYCLE_COUNT_INIT;
    z80.break_point = 0xffffffffu;
    return z80_execute();
}

void jack_cpc6128_set_matrix_key(jack_cpc6128 *machine, uint8_t row, uint8_t bit,
                                 bool pressed) {
    if(machine == NULL || row >= 16u || bit >= 8u) return;
    const uint8_t mask = (uint8_t)(1u << bit);
    if(pressed) machine->keyboard_matrix[row] &= (uint8_t)~mask;
    else machine->keyboard_matrix[row] |= mask;
}

bool jack_cpc6128_type_ascii(jack_cpc6128 *machine, uint16_t code, bool pressed) {
    static const uint8_t letter_keys[26] = {
        0x85u,0x66u,0x76u,0x75u,0x72u,0x65u,0x64u,0x54u,0x43u,
        0x55u,0x45u,0x44u,0x46u,0x56u,0x42u,0x33u,0x83u,0x62u,
        0x74u,0x63u,0x52u,0x67u,0x73u,0x77u,0x53u,0x87u
    };
    static const uint8_t digit_keys[10] = {
        0x40u,0x80u,0x81u,0x71u,0x70u,0x61u,0x60u,0x51u,0x50u,0x41u
    };
    if(machine == NULL) return false;
    bool shifted = false;
    uint8_t matrix_key = 0xffu;
    if(code >= 'A' && code <= 'Z') {
        shifted = true;
        code = (uint16_t)(code + ('a' - 'A'));
    }
    if(code >= 'a' && code <= 'z') matrix_key = letter_keys[code - 'a'];
    else if(code >= '0' && code <= '9') matrix_key = digit_keys[code - '0'];
    else {
        switch(code) {
            case JACK_KEY_UP: matrix_key=0x00u;break;
            case JACK_KEY_RIGHT: matrix_key=0x01u;break;
            case JACK_KEY_DOWN: matrix_key=0x02u;break;
            case JACK_KEY_LEFT: matrix_key=0x10u;break;
            case JACK_KEY_ENTER: matrix_key=0x22u;break;
            case JACK_KEY_ESCAPE: matrix_key=0x82u;break;
            case JACK_KEY_BACKSPACE:
            case JACK_KEY_DELETE: matrix_key=0x97u;break;
            case JACK_KEY_TAB: matrix_key=0x84u;break;
            case ' ': matrix_key=0x57u;break;
            case '[': matrix_key=0x21u;break;
            case ']': matrix_key=0x23u;break;
            case '/': matrix_key=0x26u;break;
            case '-': matrix_key=0x31u;break;
            case '@': matrix_key=0x32u;break;
            case ';': matrix_key=0x34u;break;
            case ':': matrix_key=0x35u;break;
            case '\\': matrix_key=0x36u;break;
            case '.': matrix_key=0x37u;break;
            case ',': matrix_key=0x47u;break;
            case '!': matrix_key=0x80u;shifted=true;break;
            case '"': matrix_key=0x81u;shifted=true;break;
            case '#': matrix_key=0x71u;shifted=true;break;
            case '$': matrix_key=0x70u;shifted=true;break;
            case '%': matrix_key=0x61u;shifted=true;break;
            case '&': matrix_key=0x60u;shifted=true;break;
            case '(': matrix_key=0x50u;shifted=true;break;
            case ')': matrix_key=0x41u;shifted=true;break;
            default:return false;
        }
    }
    if(pressed && shifted) jack_cpc6128_set_matrix_key(machine,2u,5u,true);
    jack_cpc6128_set_matrix_key(machine,(uint8_t)(matrix_key >> 4u),
                                matrix_key & 7u,pressed);
    if(!pressed && shifted) jack_cpc6128_set_matrix_key(machine,2u,5u,false);
    return true;
}

bool jack_cpc6128_handle_input(jack_cpc6128 *machine, const jack_input_event *event) {
    if(machine == NULL || event == NULL) return false;
    const bool pressed = event->type == JACK_INPUT_KEY_DOWN ||
                         event->type == JACK_INPUT_PAD_DOWN;
    if(!pressed && event->type != JACK_INPUT_KEY_UP && event->type != JACK_INPUT_PAD_UP)
        return false;
    if(event->type == JACK_INPUT_KEY_DOWN || event->type == JACK_INPUT_KEY_UP)
        return jack_cpc6128_type_ascii(machine,event->code,pressed);

    uint8_t bit = 0xffu;
    switch(event->code) {
        case JACK_PAD_UP: bit=0u;break;
        case JACK_PAD_DOWN: bit=1u;break;
        case JACK_PAD_LEFT: bit=2u;break;
        case JACK_PAD_RIGHT: bit=3u;break;
        case JACK_PAD_A: bit=4u;break;
        case JACK_PAD_B: bit=5u;break;
        case JACK_PAD_X: bit=6u;break;
        case JACK_PAD_START:
            return jack_cpc6128_type_ascii(machine,JACK_KEY_ENTER,pressed);
        case JACK_PAD_SELECT:
            return jack_cpc6128_type_ascii(machine,JACK_KEY_ESCAPE,pressed);
        default:return false;
    }
    jack_cpc6128_set_matrix_key(machine,9u,bit,pressed);
    return true;
}
void jack_cpc6128_palette_rgb555(const jack_cpc6128 *machine, uint16_t palette[17]) {
    if(machine == NULL || palette == NULL) return;
    for(unsigned index = 0; index < 17u; ++index) {
        palette[index] = cpc_ink_rgb555[GateArray.ink_values[index] & 0x1fu];
    }
}

void jack_cpc6128_scanline_palette_rgb555(const jack_cpc6128 *machine,
                                         unsigned y,uint16_t palette[17]) {
    if(machine==NULL||palette==NULL) return;
    if(y>=240u||machine->frame_source_y[y]>=312u) {
        jack_cpc6128_palette_rgb555(machine,palette);
        return;
    }
    for(unsigned i=0u;i<17u;++i)
        palette[i]=cpc_ink_rgb555[machine->scanline_inks[machine->frame_source_y[y]][i]&31u];
}

uint32_t jack_cpc6128_frame_hash(const jack_cpc6128 *machine) {
    if(machine == NULL || machine->config.frame_index8 == NULL) return 0u;
    uint32_t hash = 2166136261u;
    for(unsigned y = 0; y < JACK_CPC_FRAME_HEIGHT; ++y) {
        const uint8_t *row = machine->config.frame_index8 + y * machine->config.frame_stride;
        for(unsigned x = 0; x < JACK_CPC_FRAME_WIDTH; ++x) hash = (hash ^ row[x]) * 16777619u;
    }
    return hash;
}
