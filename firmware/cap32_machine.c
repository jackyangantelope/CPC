// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "tf_card.h"
#include "hstx.h"
#include "hstx_data_island_queue.h"
#include "jack_audio.h"
#include "jack_cpc6128.h"
#include "jack_cpc_dsk_persist.h"
#include "jack_fatfs_stream.h"
#include "jack_input_devices.h"
#include "jack_memory.h"
#include "jack_nespad_input.h"
#include "jack_storage_paths.h"
#include "jack_tinyusb_input.h"
#include "jack_video.h"
#include "jack_terminal_rgb555.h"
#include "cap32.h"
#include "z80.h"

extern t_z80regs z80;
extern t_CRTC CRTC;

#define CPC_DSK_PATCH_CAPACITY 32768u
#define CPC_AUTOTYPE_DELAY_FRAMES 200u

static FATFS filesystem;
static jack_cpc6128 machine;
static jack_audio_frame audio_frames[2048];
static uint8_t hot_memory[16u * 1024u];
static uint8_t system_rom_sram[3u*JACK_CPC_ROM_BYTES];
static uint8_t index_frame_sram[JACK_CPC_FRAME_BYTES];
static jack_cpc_dsk_overlay dsk_overlay;
static char dsk_overlay_path[96];
static bool dsk_overlay_ready;
static bool dsk_overlay_restored;
static uint32_t dsk_overlay_saved_generation;
static uint32_t dsk_crc32;
static char (*disk_names)[256];
static unsigned disk_count;
static uint32_t audio_submit_us;
static char runtime_log[768];
static size_t runtime_log_length,runtime_log_position;
static bool runtime_log_recording;

static void append_runtime_log(const char *format,...) {
    if(!runtime_log_recording||runtime_log_length>=sizeof(runtime_log)-1u) return;
    va_list arguments;
    va_start(arguments,format);
    const size_t room=sizeof(runtime_log)-runtime_log_length;
    const int written=vsnprintf(runtime_log+runtime_log_length,room,format,arguments);
    va_end(arguments);
    if(written>0) runtime_log_length+=(size_t)written<room?(size_t)written:room-1u;
}

static void service_runtime_log(void) {

    unsigned budget=32u;
    while(budget&&runtime_log_position<runtime_log_length&&uart_is_writable(uart_default)) {
        uart_putc_raw(uart_default,runtime_log[runtime_log_position++]);
        --budget;
    }
}

static void boot_stage(const char *stage) {
    printf("CPC_BOOT %s\n",stage);
    stdio_flush();
}

static void show_boot_error(const char *reason) {
    static jack_terminal_cell cells[40u*8u];
    jack_terminal terminal;
    (void)jack_terminal_init(&terminal,40u,8u,cells,40u*8u);
    jack_terminal_clear(&terminal,' ',15u,0u);
    (void)jack_terminal_write(&terminal,1u,1u,"CPC STARTUP ERROR",15u,0u);
    (void)jack_terminal_write(&terminal,1u,3u,reason,15u,0u);
    (void)jack_terminal_write(&terminal,1u,5u,"ROM: /roms/CPC/cpc6128.rom (32 KiB)",15u,0u);
    (void)jack_terminal_write(&terminal,1u,6u,"See UART log; reset after fixing.",15u,0u);
    memset(hstx_getframebuffer(),0,JACK_CPC_FRAME_BYTES*sizeof(uint16_t));
    (void)jack_terminal_render_rgb555(&terminal,(uint16_t *)hstx_getframebuffer(),
        JACK_CPC_FRAME_WIDTH,JACK_CPC_FRAME_HEIGHT,JACK_CPC_FRAME_WIDTH,
        jack_rgb555_palette);
    printf("CPC_BOOT ERROR: %s\n",reason);
    stdio_flush();
    while(true) tight_loop_contents();
}

static bool load_rom_file(const char *path, jack_cpc_rom_kind kind) {
    jack_stream stream;
    jack_fatfs_file file;
    if(!jack_fatfs_open_read(&stream,&file,path)) return false;
    const bool ok=jack_cpc6128_load_rom(&machine,kind,&stream);
    jack_stream_close(&stream);
    return ok;
}

static bool load_system_rom(void) {
    jack_stream stream;
    jack_fatfs_file file;
    if(jack_fatfs_open_read(&stream,&file,"/roms/CPC/cpc6128.rom")) {
        const bool ok=jack_cpc6128_load_system_rom(&machine,&stream);
        jack_stream_close(&stream);
        if(ok) return true;
    }
    return load_rom_file("/roms/CPC/os.rom",JACK_CPC_ROM_LOWER)&&
        load_rom_file("/roms/CPC/basic.rom",JACK_CPC_ROM_BASIC);
}

static bool restore_dsk_overlay(void) {
    jack_stream stream;
    jack_fatfs_file file;
    if(!jack_fatfs_open_read(&stream,&file,dsk_overlay_path)) return false;
    const bool ok=jack_cpc_dsk_overlay_load_stream(&dsk_overlay,&stream);
    jack_stream_close(&stream);
    return ok;
}

static bool load_selected_dsk(jack_memory_layout *memory,const char *path) {
    jack_stream stream;
    jack_fatfs_file file;
    if(!jack_fatfs_open_read(&stream,&file,path)) return false;
    const uint64_t length=jack_stream_size(&stream);
    if(length<0x200u||length>2u*1024u*1024u) {
        jack_stream_close(&stream);
        return false;
    }
    uint8_t *data=(uint8_t *)jack_memory_alloc_bulk(memory,(size_t)length,4u);
    const bool read_ok=data!=NULL&&jack_stream_seek(&stream,0u)&&
        jack_stream_read_exact(&stream,data,(size_t)length);
    jack_stream_close(&stream);
    if(!read_ok||!jack_cpc6128_mount_dsk(&machine,data,(size_t)length)) return false;

    jack_cpc_dsk_patch *patches=(jack_cpc_dsk_patch *)jack_memory_alloc_bulk(memory,
        CPC_DSK_PATCH_CAPACITY*sizeof(*patches),4u);
    if(patches==NULL||!jack_cpc_dsk_overlay_init(&dsk_overlay,data,(size_t)length,
       patches,CPC_DSK_PATCH_CAPACITY)||!jack_cpc6128_attach_dsk_overlay(&machine,&dsk_overlay))
        return false;
    dsk_crc32=jack_storage_crc32(0u,data,(size_t)length);
    dsk_overlay_ready=jack_storage_save_path(dsk_overlay_path,sizeof(dsk_overlay_path),
        "CPC",dsk_crc32,".jcow")&&jack_fatfs_ensure_save_dir("CPC",dsk_crc32)==FR_OK;
    if(dsk_overlay_ready) dsk_overlay_restored=restore_dsk_overlay();
    dsk_overlay_saved_generation=dsk_overlay.write_generation;
    return true;
}

static bool save_dsk_overlay_if_dirty(void) {
    if(!dsk_overlay_ready||dsk_overlay.write_generation==dsk_overlay_saved_generation)
        return true;
    jack_stream stream;
    jack_fatfs_file file;
    if(!jack_fatfs_open_write(&stream,&file,dsk_overlay_path)) return false;
    const bool ok=jack_cpc_dsk_overlay_save_stream(&dsk_overlay,&stream);
    jack_stream_close(&stream);
    if(ok) dsk_overlay_saved_generation=dsk_overlay.write_generation;
    return ok;
}

static void hstx_audio_sink(void *context,int16_t left,int16_t right) {
    (void)context;
    hstx_push_audio_sample(left,right);
}

static void service_audio(jack_audio_ring *audio) {

    const uint32_t level=hstx_di_queue_get_level();
    if(level+1u>=HSTX_AUDIO_DI_HIGH_WATERMARK) return;
    size_t count=(HSTX_AUDIO_DI_HIGH_WATERMARK-level-1u)*4u;
    if(count>audio->count) count=audio->count;
    if(count) {
        const uint32_t started=time_us_32();
        (void)jack_audio_drain(audio,count,hstx_audio_sink,NULL);
        audio_submit_us+=time_us_32()-started;
    }
}

static void route_input(jack_input_event event) {
    (void)jack_cpc6128_handle_input(&machine,&event);
}

static FRESULT scan_disks(void) {
    DIR directory;
    FILINFO file;
    FRESULT result=f_opendir(&directory,"/roms/CPC");
    if(result!=FR_OK) return result;
    while((result=f_readdir(&directory,&file))==FR_OK&&file.fname[0]) {
        const size_t n=strlen(file.fname);
        if((file.fattrib&AM_DIR)||n<5u||n>=256u) continue;
        const char *s=file.fname+n-4u;
        if(s[0]!='.'||(s[1]!='d'&&s[1]!='D')||
           (s[2]!='s'&&s[2]!='S')||(s[3]!='k'&&s[3]!='K')) continue;
        if(disk_count<64u) strcpy(disk_names[disk_count++],file.fname);
    }
    (void)f_closedir(&directory);
    for(unsigned i=1u;i<disk_count;++i) {
        char name[256];
        strcpy(name,disk_names[i]);
        unsigned j=i;
        while(j&&strcmp(disk_names[j-1u],name)>0) {
            strcpy(disk_names[j],disk_names[j-1u]);
            --j;
        }
        strcpy(disk_names[j],name);
    }
    return result;
}

static void draw_disk_menu(unsigned selected) {
    static jack_terminal_cell cells[40u*30u];
    jack_terminal terminal;
    (void)jack_terminal_init(&terminal,40u,30u,cells,40u*30u);
    jack_terminal_clear(&terminal,' ',15u,0u);
    (void)jack_terminal_write(&terminal,1u,1u,"AMSTRAD CPC - DISK LIST",15u,0u);
    (void)jack_terminal_write(&terminal,1u,3u,"/roms/CPC/*.dsk",7u,0u);
    const unsigned page=selected/16u*16u;
    for(unsigned row=0u;row<16u&&page+row<=disk_count;++row) {
        const unsigned item=page+row;
        const char *name=item?disk_names[item-1u]:"BASIC (NO DISK)";
        const uint8_t bg=item==selected?2u:0u;
        (void)jack_terminal_write(&terminal,1u,(uint16_t)(5u+row),item==selected?">":" ",15u,bg);
        for(unsigned c=0u;name[c]&&c<36u;++c)
            (void)jack_terminal_put(&terminal,(uint16_t)(3u+c),(uint16_t)(5u+row),
                (uint8_t)name[c],15u,bg);
    }
    (void)jack_terminal_write(&terminal,1u,25u,"UP/DOWN: SELECT  A/START/ENTER: LOAD",15u,0u);
    (void)jack_terminal_write(&terminal,1u,27u,"DISK COMMAND: RUN\"DISC\"",7u,0u);
    (void)jack_terminal_render_rgb555(&terminal,(uint16_t *)hstx_getframebuffer(),
        320u,240u,320u,jack_rgb555_palette);
}

static unsigned select_disk(jack_input_devices *devices,jack_input_queue *queue,
                            bool usb_ok,bool nes_ok) {
    unsigned selected=disk_count?1u:0u;
    uint16_t previous=0xffffu;
    const uint64_t input_after=time_us_64()+300000u;
    uint64_t report_after=time_us_64()+1000000u;
    uint32_t last_frame=hstx_getframecounter();
    draw_disk_menu(selected);
    while(true) {
        if(usb_ok) jack_tinyusb_input_poll();
        if(nes_ok) (void)jack_nespad_input_poll();
        uint16_t keys=0u;
        jack_input_event event;
        while(jack_input_pop(queue,&event)) {
            if(event.type!=JACK_INPUT_KEY_DOWN) continue;
            if(event.code==JACK_KEY_UP) keys|=JACK_PAD_UP;
            if(event.code==JACK_KEY_DOWN) keys|=JACK_PAD_DOWN;
            if(event.code==JACK_KEY_ENTER) keys|=JACK_PAD_START;
        }
        const uint16_t buttons=devices->pads[0];
        uint16_t pressed=(uint16_t)((buttons&~previous)|keys);
        previous=buttons;
        if(time_us_64()<input_after) pressed=0u;
        if(pressed&JACK_PAD_UP) selected=selected?selected-1u:disk_count;
        if(pressed&JACK_PAD_DOWN) selected=(selected+1u)%(disk_count+1u);
        if(pressed&(JACK_PAD_A|JACK_PAD_START)) return selected;
        if(pressed) draw_disk_menu(selected);
        const uint64_t now=time_us_64();
        if(now>=report_after) {
            const uint32_t frame=hstx_getframecounter();
            printf("CPC_MENU disks=%u selected=%u hstx_delta=%lu sys=%lu hstx=%lu\n",
                disk_count,selected,(unsigned long)(frame-last_frame),
                (unsigned long)clock_get_hz(clk_sys),(unsigned long)clock_get_hz(clk_hstx));
            last_frame=frame;
            report_after=now+1000000u;
        }
        tight_loop_contents();
    }
}

static bool autotype_disk_command(bool enabled) {
    static const uint16_t command[]={'r','u','n','"','d','i','s','c','"',JACK_KEY_ENTER};
    static size_t position;
    static uint32_t previous_frame=0xffffffffu;
    if(!enabled||position>=sizeof(command)/sizeof(command[0])) return true;
    if(machine.frame_count<CPC_AUTOTYPE_DELAY_FRAMES) return false;
    if(previous_frame==machine.frame_count) return false;
    previous_frame=machine.frame_count;
    const unsigned phase=(machine.frame_count-CPC_AUTOTYPE_DELAY_FRAMES)%4u;

    if(phase==0u||phase==2u) {
        const bool pressed=phase==0u;
        jack_input_event event={pressed?JACK_INPUT_KEY_DOWN:JACK_INPUT_KEY_UP,0u,
            command[position],pressed?1:0};
        route_input(event);
        if(!pressed) ++position;
    }
    return position>=sizeof(command)/sizeof(command[0]);
}

static void show_frame(void) {
    uint16_t palette[17];
    uint16_t *destination=(uint16_t *)hstx_getframebuffer();
    for(unsigned y=0u;y<JACK_CPC_FRAME_HEIGHT;++y) {
        jack_cpc6128_scanline_palette_rgb555(&machine,y,palette);
        const uint8_t *source=machine.config.frame_index8+y*machine.config.frame_stride;
        for(unsigned x=0u;x<JACK_CPC_FRAME_WIDTH;++x)
            destination[y*JACK_CPC_FRAME_WIDTH+x]=palette[source[x]<=16u?source[x]:16u];
    }
}

int main(void) {
    stdio_init_all();
    boot_stage("0.3.9 Fruit Jam startup; clock begin");
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    if(!set_sys_clock_khz(252000,true)) return 2;
    stdio_init_all();
    sleep_ms(200);
    boot_stage("clock ready; psram begin");

    const bool psram_present=psram_is_available();
    const int psram_params=psram_present?psram_configure_params(
        PICO_DEFAULT_PSRAM_MAX_FREQ,PICO_DEFAULT_PSRAM_MAX_SELECT,
        PICO_DEFAULT_PSRAM_MIN_DESELECT):PICO_ERROR_NOT_FOUND;

    // QMI reinit must finish before HSTX starts.
    const uint32_t irq_state=save_and_disable_interrupts();
    const int psram_reinit=psram_params==PICO_OK?psram_reinitialize():psram_params;
    restore_interrupts(irq_state);
    const bool psram_ok=psram_present&&psram_params==PICO_OK&&psram_reinit==PICO_OK;
    const size_t psram_size=psram_get_size();
    printf("CPC_BOOT psram present=%u params=%d reinit=%d bytes=%lu\n",
        psram_present?1u:0u,psram_params,psram_reinit,(unsigned long)psram_size);
    stdio_flush();
    jack_memory_layout memory;
    const bool memory_ok=psram_ok&&jack_memory_init(&memory,hot_memory,sizeof(hot_memory),
        (void *)0x11000000u,psram_size);

    pico_fatfs_spi_config_t sd_config={SDCARD_SPI,CLK_SLOW_DEFAULT,CLK_FAST_DEFAULT_PIO,
        SDCARD_PIN_MISO,SDCARD_PIN_CS,SDCARD_PIN_SCK,SDCARD_PIN_MOSI,true};
    if(!pico_fatfs_set_config(&sd_config)) panic("CPC SD pins rejected");
    boot_stage("sd configured; mount begin");
    const FRESULT sd_result=jack_fatfs_mount(&filesystem);
    printf("CPC_BOOT sd result=%d\n",(int)sd_result);
    stdio_flush();
    boot_stage("hdmi begin");
    hstx_init(false);
    boot_stage("hdmi ready");

    if(!memory_ok) show_boot_error("PSRAM initialization failed");
    if(sd_result!=FR_OK) show_boot_error("SD mount failed");
    boot_stage("machine and ROM load begin");
    uint8_t *ram=(uint8_t *)jack_memory_alloc_bulk(&memory,JACK_CPC_RAM_BYTES,4u);
    uint8_t *lower=system_rom_sram;
    uint8_t *basic=system_rom_sram+JACK_CPC_ROM_BYTES;
    uint8_t *amsdos=system_rom_sram+2u*JACK_CPC_ROM_BYTES;
    uint8_t *index_frame=index_frame_sram;
    jack_audio_ring audio;
    const bool audio_ok=jack_audio_init(&audio,audio_frames,
        sizeof(audio_frames)/sizeof(audio_frames[0]),JACK_CPC_AUDIO_RATE);
    const jack_cpc6128_config config={ram,JACK_CPC_RAM_BYTES,lower,basic,amsdos,
        index_frame,JACK_CPC_FRAME_WIDTH,&audio};
    const bool machine_ok=ram&&lower&&basic&&amsdos&&index_frame&&audio_ok&&
        jack_cpc6128_init(&machine,&config);
    const bool system_rom_ok=machine_ok&&sd_result==FR_OK&&load_system_rom();
    const bool amsdos_ok=machine_ok&&sd_result==FR_OK&&
        load_rom_file("/roms/CPC/amsdos.rom",JACK_CPC_ROM_AMSDOS);
    bool dsk_ok=false;
    if(machine_ok) jack_cpc6128_reset(&machine);
    printf("CPC_BOOT machine=%u system_rom=%u amsdos=%u dsk=%u\n",
        machine_ok?1u:0u,system_rom_ok?1u:0u,amsdos_ok?1u:0u,dsk_ok?1u:0u);
    stdio_flush();
    if(!machine_ok) show_boot_error("CPC memory allocation failed");
    if(!system_rom_ok) show_boot_error("System ROM missing or invalid size");
    boot_stage("input begin");

    jack_input_queue queue;
    jack_input_devices devices;
    jack_input_init(&queue);
    const bool input_ok=jack_input_devices_init(&devices,&queue);
    const bool usb_ok=input_ok&&jack_tinyusb_input_init(&devices);
    const bool nes_ok=input_ok&&jack_nespad_input_init(&devices,252000u);

    disk_names=(char (*)[256])jack_memory_alloc_bulk(&memory,64u*256u,4u);
    if(!disk_names) show_boot_error("Disk list allocation failed");
    const FRESULT scan_result=scan_disks();
    printf("CPC_BOOT disk scan=%d disks=%u sys=%lu hstx=%lu\n",(int)scan_result,
        disk_count,(unsigned long)clock_get_hz(clk_sys),(unsigned long)clock_get_hz(clk_hstx));
    stdio_flush();
    if(scan_result!=FR_OK) show_boot_error("Cannot list /roms/CPC");
    const unsigned selected=select_disk(&devices,&queue,usb_ok,nes_ok);
    if(selected) {
        char path[272];
        (void)snprintf(path,sizeof(path),"/roms/CPC/%s",disk_names[selected-1u]);
        printf("CPC_BOOT selected=%s\n",path);
        stdio_flush();
        dsk_ok=load_selected_dsk(&memory,path);
        if(!dsk_ok) show_boot_error("Disk load failed or unsupported DSK");
    }
    jack_cpc6128_reset(&machine);

    printf("AMSTRAD_CPC6128 psram=%u size=%lu sd=%d machine=%u system_rom=%u "
           "amsdos=%u dsk=%u cow=%u restored=%u crc=%08lx usb=%u nes=%u app=%08x\n",
        psram_ok?1u:0u,(unsigned long)psram_size,(int)sd_result,machine_ok?1u:0u,
        system_rom_ok?1u:0u,amsdos_ok?1u:0u,dsk_ok?1u:0u,dsk_overlay_ready?1u:0u,
        dsk_overlay_restored?1u:0u,(unsigned long)dsk_crc32,usb_ok?1u:0u,nes_ok?1u:0u,
#ifdef JACK_APP_BASE
        (unsigned int)JACK_APP_BASE
#else
        0u
#endif
    );

    show_frame();
    uint64_t next_cpc_frame=time_us_64();
    uint64_t next_report=time_us_64()+1000000u;
    uint64_t report_started=time_us_64();
    uint32_t run_us=0u,blit_us=0u,max_work_us=0u,work_frames=0u,audio_waits=0u;
    uint32_t report_video_us=machine.video_us,report_audio_us=machine.audio_us;
    uint32_t report_di_under=hstx_di_queue_get_underrun_count();
    uint32_t report_frames=machine.frame_count;
    uint32_t report_hstx=hstx_getframecounter();
    bool autotype_pending=dsk_ok&&amsdos_ok;
    while(true) {
        if(usb_ok) jack_tinyusb_input_poll();
        if(nes_ok) (void)jack_nespad_input_poll();
        jack_input_event event;
        while(jack_input_pop(&queue,&event)) {

            if(!autotype_pending) route_input(event);
        }
        service_audio(&audio);
        service_runtime_log();

        const uint64_t frame_now=time_us_64();
        if(frame_now>=next_cpc_frame&&audio.capacity-audio.count>=882u) {
            next_cpc_frame+=20000u;
            if(next_cpc_frame+20000u<frame_now) next_cpc_frame=frame_now+20000u;
            if(autotype_pending) autotype_pending=!autotype_disk_command(true);
            const uint32_t began=time_us_32();
            (void)jack_cpc6128_run_frame(&machine);
            const uint32_t core_done=time_us_32();
            show_frame();
            const uint32_t work_done=time_us_32();
            run_us+=core_done-began;
            blit_us+=work_done-core_done;
            if(work_done-began>max_work_us) max_work_us=work_done-began;
            ++work_frames;
        } else if(frame_now>=next_cpc_frame) {
            ++audio_waits;
        }
        service_audio(&audio);

        const uint64_t now=time_us_64();
        if(now>=next_report) {
            runtime_log_recording=runtime_log_position==runtime_log_length;
            if(runtime_log_recording) runtime_log_length=runtime_log_position=0u;
            const bool save_ok=save_dsk_overlay_if_dirty();
            uint32_t nonblack=0u;
            const uint16_t *pixels=(const uint16_t *)hstx_getframebuffer();
            for(unsigned i=0u;i<JACK_CPC_FRAME_BYTES;++i) if(pixels[i]) ++nonblack;
            const uint32_t hstx_frames=hstx_getframecounter();
            const uint64_t elapsed=now-report_started;
            const uint32_t video_us=machine.video_us-report_video_us;
            const uint32_t synth_us=machine.audio_us-report_audio_us;
            const uint32_t di_under=hstx_di_queue_get_underrun_count();
            append_runtime_log("CPC_PERF fps10=%lu hdmi10=%lu core_us=%lu index_us=%lu ay_us=%lu blit_us=%lu pump_us=%lu max_us=%lu di=%lu di_under=%lu wait=%lu\r\n",
                (unsigned long)((machine.frame_count-report_frames)*10000000ull/elapsed),
                (unsigned long)((hstx_frames-report_hstx)*10000000ull/elapsed),
                (unsigned long)(work_frames?(run_us-video_us-synth_us)/work_frames:0u),
                (unsigned long)(work_frames?video_us/work_frames:0u),
                (unsigned long)(work_frames?synth_us/work_frames:0u),
                (unsigned long)(work_frames?blit_us/work_frames:0u),
                (unsigned long)(work_frames?audio_submit_us/work_frames:0u),(unsigned long)max_work_us,
                (unsigned long)hstx_di_queue_get_level(),(unsigned long)(di_under-report_di_under),
                (unsigned long)audio_waits);
            report_started=now;
            report_video_us=machine.video_us;report_audio_us=machine.audio_us;
            report_di_under=di_under;
            run_us=blit_us=max_work_us=work_frames=audio_waits=0u;
            audio_submit_us=0u;
            append_runtime_log("CPC_VIDEO pc=%04x crtc=%02x%02x mode=%u fps=%lu hstx_delta=%lu nonblack=%lu\r\n",
                z80.PC.w.l,CRTC.registers[12],CRTC.registers[13],machine.current_mode,
                (unsigned long)(machine.frame_count-report_frames),
                (unsigned long)(hstx_frames-report_hstx),(unsigned long)nonblack);
            report_frames=machine.frame_count;
            report_hstx=hstx_frames;
            append_runtime_log("AMSTRAD_CPC6128_ALIVE frames=%lu irq=%lu io=%lu/%lu audio=%lu "
                   "under=%lu over=%lu cow=%lu/%lu save=%u psram=%lu\r\n",
                (unsigned long)machine.frame_count,(unsigned long)machine.interrupt_count,
                (unsigned long)machine.io_reads,(unsigned long)machine.io_writes,
                (unsigned long)audio.count,(unsigned long)audio.underruns,
                (unsigned long)audio.overruns,(unsigned long)dsk_overlay.patch_count,
                (unsigned long)dsk_overlay.write_generation,save_ok?1u:0u,
                (unsigned long)memory.bulk.high_water);
            next_report=now+1000000u;
        }
        tight_loop_contents();
    }
}
