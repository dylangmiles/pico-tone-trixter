cmake_minimum_required(VERSION 3.13...3.29)

include(pico_sdk_import.cmake)

project(pico_tone_trixter C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---- FFTConvolver static library (Ooura FFT backend, bundled in AudioFFT) ----
add_library(fft_convolver STATIC
    lib/FFTConvolver/FFTConvolver.cpp
    lib/FFTConvolver/TwoStageFFTConvolver.cpp
    lib/FFTConvolver/AudioFFT.cpp
    lib/FFTConvolver/Utilities.cpp
)
target_include_directories(fft_convolver PUBLIC lib/FFTConvolver)
# Disable asserts: on newlib/embedded an assert failure calls abort() which silently
# spins with no output, making it look like a hang with no panic.
target_compile_definitions(fft_convolver PRIVATE NDEBUG)
# pico_float/pico_double route float/double ops through RP2040 ROM routines.
# The Ooura FFT backend in AudioFFT uses double internally, so pico_double is required
# to avoid catastrophically slow soft-float 64-bit arithmetic (libgcc fallback).
target_link_libraries(fft_convolver pico_float pico_double)

# ---- Main executable ----
add_executable(pico_tone_trixter
    main.cpp
    i2s/i2s.c
    audio/pipeline.cpp
    audio/dsp.cpp
)

# Generate C header from the PIO assembly
pico_generate_pio_header(pico_tone_trixter
    ${CMAKE_CURRENT_LIST_DIR}/i2s/i2s_out.pio
)

# Project root on the include path so all files can use "i2s/i2s.h", "audio/pipeline.h", etc.
target_include_directories(pico_tone_trixter PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(pico_tone_trixter
    pico_stdlib
    pico_multicore
    pico_float
    hardware_pwm
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_watchdog
    fft_convolver
)

# Copy entire binary to SRAM at boot — eliminates XIP flash latency on Core 1.
# text=96KB + bss=54KB = 150KB, well within the 264KB SRAM limit.
pico_set_binary_type(pico_tone_trixter copy_to_ram)

pico_add_extra_outputs(pico_tone_trixter)

pico_enable_stdio_usb(pico_tone_trixter 0)
pico_enable_stdio_uart(pico_tone_trixter 1)