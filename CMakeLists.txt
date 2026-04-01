cmake_minimum_required(VERSION 3.13...3.29)

# Must be set before include(pico_sdk_import.cmake) — SDK reads it during toolchain setup
set(PICO_BOARD pico2)

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
# RP2350 (Cortex-M33) has hardware FPU — pico_float/pico_double are not needed.

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

# ============================================================
# Offline IR test — processes embedded audio and streams WAV
# over USB CDC for host capture via tools/capture_wav.py
# Does NOT use copy_to_ram: the 1.5 MB piezo binary lives in
# flash and is accessed via XIP.
# ============================================================

# Step 1: Python script converts WAV files → ir_array.h + piezo_raw.bin
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SAMPLES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/audio/samples)
add_custom_command(
    OUTPUT
        ${SAMPLES_DIR}/ir_array.h
        ${SAMPLES_DIR}/piezo_raw.bin
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_audio_arrays.py
            ${SAMPLES_DIR}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_audio_arrays.py
        ${SAMPLES_DIR}/IR_garrison-NT1-A-20260320_48k_2048_M.wav
        ${SAMPLES_DIR}/garrison-piezo-20260320.wav
    COMMENT "Generating audio C arrays from WAV files"
)
add_custom_target(audio_arrays DEPENDS
    ${SAMPLES_DIR}/ir_array.h
    ${SAMPLES_DIR}/piezo_raw.bin
)

# Step 2: Embed piezo_raw.bin as a .rodata section using objcopy.
# Symbol names: _binary_piezo_raw_bin_{start,end,size}
set(PIEZO_OBJ ${CMAKE_CURRENT_BINARY_DIR}/piezo_raw.o)
add_custom_command(
    OUTPUT ${PIEZO_OBJ}
    COMMAND ${CMAKE_OBJCOPY}
            -I binary -O elf32-littlearm -B armv8-m.main
            --rename-section .data=.rodata,alloc,load,readonly,data,contents
            piezo_raw.bin ${PIEZO_OBJ}
    WORKING_DIRECTORY ${SAMPLES_DIR}
    DEPENDS ${SAMPLES_DIR}/piezo_raw.bin
    COMMENT "Embedding piezo audio binary"
)

# Step 3: offline_test executable
option(OFFLINE_TEST_TWO_STAGE
    "Use TwoStageFFTConvolver in offline_test (head=64, tail=2048) instead of FFTConvolver (block=256)"
    OFF)

add_executable(offline_test
    audio/offline_test.cpp
    ${PIEZO_OBJ}
)
add_dependencies(offline_test audio_arrays)
target_include_directories(offline_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(offline_test
    pico_stdlib
    fft_convolver
)
if(OFFLINE_TEST_TWO_STAGE)
    target_compile_definitions(offline_test PRIVATE USE_TWO_STAGE=1)
    message(STATUS "offline_test: TwoStageFFTConvolver (head=64 tail=2048)")
else()
    message(STATUS "offline_test: FFTConvolver (block=256)")
endif()
pico_add_extra_outputs(offline_test)
pico_enable_stdio_usb(offline_test 0)
pico_enable_stdio_uart(offline_test 1)