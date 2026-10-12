# CPC

Amstrad CPC 6128 emulator source port for the public Adafruit Fruit Jam (RP2350, `HW_CONFIG=8`). This is a source-only release candidate. It contains no Amstrad system ROM, game disk, save, or prebuilt UF2.

## Status

The 0.3.9 baseline passed a user-confirmed basic hardware test with Return Home: boot, standard keyboard input, picture and sound worked. The last 30 steady-state samples averaged 49.913 FPS (49.9–50.2). One sample during a loading-screen transition recorded 33 HDMI data-island underruns; the steady game samples recorded none. This does not establish full CPC compatibility or long-run stability.

The 0.3.9 source is configured here for Fruit Jam at 252 MHz. This variant **has been compiled but not tested on Fruit Jam hardware**. USB keyboard input is included; NES/SNES controller GPIO input is not enabled. Do not assume the above hardware result transfers to Fruit Jam unchanged.

## Build

Use Pico SDK 2.3.x with RP2350/Fruit Jam and application linker-script override support, Pico-PIO-USB, an ARM embedded toolchain, CMake and Ninja. On Windows:

```powershell
.\scripts\build-release.ps1 `
  -PicoSdkPath D:\path\to\pico-sdk `
  -PicoPioUsbPath D:\path\to\Pico-PIO-USB `
  -ToolchainPath C:\path\to\arm-toolchain `
  -NinjaPath C:\path\to\ninja.exe
```

Optional `-PioasmDir`, `-PicotoolDir`, and `-TinyUsbPath` select host tools when your SDK setup requires them. The output is `build/fruit-jam-release/picoCPC.uf2`. It is linked at `0x10080000` for a compatible resident loader, **not** a standalone UF2 to flash directly. Confirm the loader partition layout before using it.

Supply your own legally obtained CPC 6128 system ROMs on the SD card: `/roms/CPC/cpc6128.rom` (32 KiB combined OS/BASIC), or `/roms/CPC/os.rom` and `/roms/CPC/basic.rom` (16 KiB each). For disk commands, also provide `/roms/CPC/amsdos.rom` (16 KiB). Put your own `.dsk` files in `/roms/CPC/`. Disk changes are stored separately in `/SAVES/CPC/*.jcow`; the original DSK is not modified. The selected disk can be auto-started with `RUN"DISC"` when the required ROMs are present. Proprietary ROMs and game data are never provided here.

## Source and licensing

The Z80 core is an unmodified source subset of [libretro/libretro-cap32](https://github.com/libretro/libretro-cap32) revision `e9ad1826aafa458497eddc92d73491123b902fc0`: `cap32/z80.c`, `z80.h`, `z80daa.h`, `cap32.h`, `tape.h`, and `asic.h`. Its GPL-2.0-or-later source headers and original [COPYING.txt](deps/libretro-cap32/cap32/COPYING.txt) are preserved. No `cap32/rom` headers or other embedded ROM data were copied. Required `libretro-common/include` headers are carried with their own per-file copyright and license notices.

The storage and HDMI dependencies come from [PicoPlus-devel/pico_shared](https://github.com/PicoPlus-devel/pico_shared) revision `df683bf00ff95e1ceda88f132bbf72f8cc7cadb0`, with its [GPLv3 license](deps/pico_shared/LICENSE) and the separate [pico_fatfs license](deps/pico_shared/drivers/pico_fatfs/LICENSE). `platform/hdmi/video_output.c` is an adapted derivative, not a pristine copy. `runtime/` adds the CPC 6128 machine, 128 KiB memory banking, GA/CRTC/PPI/AY and DSK/COW integration without bundling ROMs. `platform/hal/` is port glue; its BSD-3-Clause and MIT notices are in [LICENSES](LICENSES). See [THIRD_PARTY.md](THIRD_PARTY.md) for the component map. The combined firmware is distributed under the repository [GPLv3](LICENSE), subject to the retained component notices.

Pico SDK, Pico-PIO-USB, TinyUSB and their dependencies are supplied by the local build environment, not copied into this repository.
