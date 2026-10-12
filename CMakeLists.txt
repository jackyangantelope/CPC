cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Adafruit Fruit Jam" FORCE)
set(PICO_PSRAM_CS_PIN 47 CACHE STRING "Fruit Jam PSRAM CS" FORCE)
set(PICO_PSRAM_SIZE_BYTES 0x800000 CACHE STRING "Fruit Jam PSRAM size" FORCE)
set(PICO_DEFAULT_UART 0 CACHE STRING "Diagnostic UART" FORCE)
set(PICO_DEFAULT_UART_TX_PIN 44 CACHE STRING "Diagnostic UART TX" FORCE)
set(PICO_DEFAULT_UART_RX_PIN 45 CACHE STRING "Diagnostic UART RX" FORCE)
if(NOT DEFINED PICO_SDK_PATH OR NOT DEFINED PICO_PIO_USB_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH and PICO_PIO_USB_PATH")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(picoCPC C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(CPC_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(CAP32_ROOT "${CPC_ROOT}/deps/libretro-cap32")
set(PICO_SHARED_PATH "${CPC_ROOT}/deps/pico_shared" CACHE PATH "Public pico_shared source")
set(FATFS_ROOT "${PICO_SHARED_PATH}/drivers/pico_fatfs")
set(HDMI_ROOT "${PICO_SHARED_PATH}/drivers/pico_hdmi")
set(HAL_ROOT "${CPC_ROOT}/platform/hal")

set(SAVED_TINYUSB_TOP "${TOP}")
set(TOP "${PICO_SDK_PATH}/lib/tinyusb")
check_and_add_pico_pio_usb_support()
set(TOP "${SAVED_TINYUSB_TOP}")
add_subdirectory("${FATFS_ROOT}" "${CMAKE_BINARY_DIR}/pico_fatfs")

add_library(cpc_hal STATIC
    "${HAL_ROOT}/audio/jack_audio.c"
    "${HAL_ROOT}/input/jack_input.c"
    "${HAL_ROOT}/input/jack_input_devices.c"
    "${HAL_ROOT}/psram/jack_memory.c"
    "${HAL_ROOT}/storage/jack_stream.c"
    "${HAL_ROOT}/storage/jack_storage_paths.c"
    "${HAL_ROOT}/text/jack_terminal.c"
    "${HAL_ROOT}/video/jack_terminal_rgb555.c"
    "${HAL_ROOT}/video/jack_video.c")
target_include_directories(cpc_hal PUBLIC
    "${HAL_ROOT}/audio" "${HAL_ROOT}/input" "${HAL_ROOT}/psram"
    "${HAL_ROOT}/storage" "${HAL_ROOT}/text" "${HAL_ROOT}/video")
target_compile_options(cpc_hal PRIVATE -O3 -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
target_link_libraries(cpc_hal PUBLIC pico_stdlib)

add_library(cpc_machine STATIC
    "${CAP32_ROOT}/cap32/z80.c"
    "${CPC_ROOT}/runtime/jack_cpc6128.c"
    "${CPC_ROOT}/runtime/jack_ay8912.c"
    "${CPC_ROOT}/runtime/jack_cpc_dsk_overlay.c"
    "${CPC_ROOT}/runtime/jack_cpc_dsk_persist.c")
target_include_directories(cpc_machine PUBLIC
    "${CAP32_ROOT}/cap32" "${CAP32_ROOT}/libretro-common/include"
    "${CPC_ROOT}/runtime")
target_compile_definitions(cpc_machine PRIVATE HAVE_STDINT_H=1 INLINE=inline)
target_compile_options(cpc_machine PRIVATE -O3 -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
target_link_libraries(cpc_machine PUBLIC cpc_hal pico_stdlib)

add_library(cpc_hdmi STATIC
    "${HDMI_ROOT}/hstx.c"
    "${CPC_ROOT}/platform/hdmi/video_output.c"
    "${HDMI_ROOT}/hstx_data_island_queue.c"
    "${HDMI_ROOT}/hstx_packet.c")
target_include_directories(cpc_hdmi PUBLIC
    "${CPC_ROOT}/platform/hdmi" "${HDMI_ROOT}")
target_compile_definitions(cpc_hdmi PRIVATE
    GPIOHSTXD0=15 GPIOHSTXD1=17 GPIOHSTXD2=19 GPIOHSTXCK=13
    GPIOHSTXINVERTED=1 JACK_HSTX_126MHZ=1)
target_compile_options(cpc_hdmi PRIVATE -O3 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter)
target_link_libraries(cpc_hdmi PUBLIC pico_stdlib pico_multicore hardware_dma hardware_sync
    hardware_clocks hardware_vreg hardware_pio hardware_spi hardware_i2c hardware_pwm hardware_adc)

add_executable(picoCPC
    "${CPC_ROOT}/firmware/cap32_machine.c"
    "${HAL_ROOT}/input/jack_tinyusb_input.c"
    "${HAL_ROOT}/input/jack_nespad_input.c"
    "${HAL_ROOT}/storage/jack_fatfs_stream.c")
pico_set_program_name(picoCPC "picoCPC")
pico_set_program_version(picoCPC "0.3.9-fruit-jam-source-candidate")
pico_enable_stdio_uart(picoCPC 1)
pico_enable_stdio_usb(picoCPC 0)
target_include_directories(picoCPC PRIVATE
    "${HAL_ROOT}/input" "${HAL_ROOT}/storage" "${FATFS_ROOT}/fatfs")
target_compile_definitions(picoCPC PRIVATE
    HW_CONFIG=8 PICO_STACK_SIZE=4096 PICO_PIO_USE_GPIO_BASE=1
    CFG_TUH_RPI_PIO_USB=1 PIO_USB_USE_PIO=2 PICO_DEFAULT_PIO_USB_DP_PIN=1
    SDCARD_PIN_CS=39 SDCARD_PIN_SCK=34 SDCARD_PIN_MOSI=35 SDCARD_PIN_MISO=36
    SDCARD_SPI=spi0 SDCARD_PIO=pio0)
target_compile_options(picoCPC PRIVATE -Wall -Wextra -Werror
    -Wno-unused-parameter -Wno-cast-function-type -Wno-maybe-uninitialized
    -Wno-attributes -Wno-sign-compare)
target_link_libraries(picoCPC PRIVATE
    cpc_machine cpc_hal cpc_hdmi pico_fatfs pico_stdlib pico_multicore
    hardware_dma hardware_pio hardware_spi hardware_clocks hardware_vreg
    hardware_psram hardware_xip_cache tinyusb_host tinyusb_pico_pio_usb)
target_link_options(picoCPC PRIVATE -Wl,--gc-sections -Wl,--print-memory-usage)
include("${CPC_ROOT}/cmake/LoaderPartition.cmake")
cpc_link_for_loader(picoCPC)
pico_add_extra_outputs(picoCPC)
