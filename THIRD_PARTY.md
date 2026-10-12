# Source provenance

| Component | Source and revision | License and local status |
| --- | --- | --- |
| `deps/libretro-cap32/cap32` | [libretro/libretro-cap32](https://github.com/libretro/libretro-cap32/commit/e9ad1826aafa458497eddc92d73491123b902fc0), `e9ad1826aafa458497eddc92d73491123b902fc0` | GPL-2.0-or-later per file headers. Original `COPYING.txt` retained. Only the six Z80-related source/header files are copied. |
| `deps/libretro-cap32/libretro-common/include` | Header snapshot from the same libretro-cap32 revision | Individual file notices retained, including MIT-style notices in the RetroArch headers. No implementation library is bundled. |
| `deps/pico_shared` | [PicoPlus-devel/pico_shared](https://github.com/PicoPlus-devel/pico_shared/commit/df683bf00ff95e1ceda88f132bbf72f8cc7cadb0), `df683bf00ff95e1ceda88f132bbf72f8cc7cadb0` | GPLv3 in `LICENSE`; `pico_fatfs` also carries its own `LICENSE`. Source snapshot. |
| `platform/hdmi` | `pico_shared` HDMI driver adapted for 126 MHz HSTX audio operation | Modified derivative, not a pristine upstream file; GPLv3 source notice and upstream license retained. |
| `runtime`, `firmware` | CPC-specific machine and public board integration | GPL-2.0-or-later source notices where marked; combined firmware is GPLv3. |
| `platform/hal` | Local audio, storage, input, memory, text and video glue | BSD-3-Clause except the MIT-marked TinyUSB config header. See `LICENSES/`. |

The Fruit Jam integration uses its public board configuration, SD/HDMI/USB pins and keyboard input. It has been compile-tested, not Fruit Jam hardware-tested. No Amstrad/Locomotive ROM, AMSDOS, CP/M, DSK/CDT, game asset, or save is shipped.
