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
# pico_float routes float ops through RP2040 ROM routines (~4x faster than libgcc soft-float)
target_link_libraries(fft_convolver pico_float)

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
    fft_convolver
)

pico_add_extra_outputs(pico_tone_trixter)

pico_enable_stdio_usb(pico_tone_trixter 0)
pico_enable_stdio_uart(pico_tone_trixter 1)