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

# ---- Product firmware: pico_tone_trixter ----
# The product (ES8388 live chain → dual-core 2048-tap IR → DSP chain → tuner →
# presets, built from main.cpp) is defined further down — it's the promoted former
# `es8388_test` target. The old sine-fed scaffold (the previous main.cpp +
# audio/pipeline.cpp + audio/dsp.cpp) was retired 2026-06-09 in the pipeline refactor.

# ============================================================
# Offline IR test — processes embedded audio and streams WAV
# over USB CDC for host capture via tools/capture_wav.py
# Does NOT use copy_to_ram: the piezo binary lives in flash
# and is accessed via XIP.
# ============================================================

set(OFFLINE_GUITAR "garrison" CACHE STRING "Guitar for offline test: garrison or tanglewood")
set(OFFLINE_DURATION "20" CACHE STRING "Seconds of piezo audio to embed (keep under ~18 to fit in 4MB flash)")

# Step 1: Python script converts WAV files → ir_array.h + piezo_raw.bin
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SAMPLES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/audio/samples)
set(IR_ARRAY_H  ${SAMPLES_DIR}/ir_array_${OFFLINE_GUITAR}.h)
set(PIEZO_BIN   ${SAMPLES_DIR}/piezo_raw_${OFFLINE_GUITAR}.bin)

add_custom_command(
    OUTPUT
        ${IR_ARRAY_H}
        ${PIEZO_BIN}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_audio_arrays.py
            ${SAMPLES_DIR}
            --guitar ${OFFLINE_GUITAR}
            --duration ${OFFLINE_DURATION}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_audio_arrays.py
        ${SAMPLES_DIR}/IR_${OFFLINE_GUITAR}-NT1-A-20260320_48k_2048_M.wav
        ${SAMPLES_DIR}/${OFFLINE_GUITAR}-piezo-20260320.wav
    COMMENT "Generating audio arrays: ${OFFLINE_GUITAR}, ${OFFLINE_DURATION}s"
)
add_custom_target(audio_arrays DEPENDS ${IR_ARRAY_H} ${PIEZO_BIN})

# Step 2: Embed piezo_raw_<guitar>.bin as a .rodata section using objcopy.
# --redefine-sym normalises the symbol names to _binary_piezo_raw_bin_{start,end,size}
# so offline_test.cpp is independent of which guitar is selected.
set(PIEZO_OBJ ${CMAKE_CURRENT_BINARY_DIR}/piezo_raw_${OFFLINE_GUITAR}.o)
add_custom_command(
    OUTPUT ${PIEZO_OBJ}
    COMMAND ${CMAKE_OBJCOPY}
            -I binary -O elf32-littlearm -B armv8-m.main
            --rename-section .data=.rodata,alloc,load,readonly,data,contents
            --redefine-sym _binary_piezo_raw_${OFFLINE_GUITAR}_bin_start=_binary_piezo_raw_bin_start
            --redefine-sym _binary_piezo_raw_${OFFLINE_GUITAR}_bin_end=_binary_piezo_raw_bin_end
            --redefine-sym _binary_piezo_raw_${OFFLINE_GUITAR}_bin_size=_binary_piezo_raw_bin_size
            piezo_raw_${OFFLINE_GUITAR}.bin ${PIEZO_OBJ}
    WORKING_DIRECTORY ${SAMPLES_DIR}
    DEPENDS ${PIEZO_BIN}
    COMMENT "Embedding ${OFFLINE_GUITAR} piezo audio binary"
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
target_compile_definitions(offline_test PRIVATE
    OFFLINE_IR_HEADER="audio/samples/ir_array_${OFFLINE_GUITAR}.h"
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

# ============================================================
# PCM1808 ADC validation test — captures live I2S audio and
# streams it as WAV over UART for host-side analysis.
# ============================================================
add_executable(pcm1808_test
    audio/pcm1808_test.cpp
)
pico_generate_pio_header(pcm1808_test
    ${CMAKE_CURRENT_LIST_DIR}/i2s/i2s_in.pio
)
target_include_directories(pcm1808_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(pcm1808_test
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_resets
)
pico_add_extra_outputs(pcm1808_test)
pico_enable_stdio_usb(pcm1808_test 0)
pico_enable_stdio_uart(pcm1808_test 1)

# ============================================================
# Transparent passthrough test — PCM1808 ADC → Pico 2 → CS4344 DAC.
# Validates the complete real-time signal chain with no DSP processing.
# ============================================================
add_executable(passthrough_test
    audio/passthrough_test.cpp
    i2s/i2s.c
)
pico_generate_pio_header(passthrough_test
    ${CMAKE_CURRENT_LIST_DIR}/i2s/i2s_out.pio
)
pico_generate_pio_header(passthrough_test
    ${CMAKE_CURRENT_LIST_DIR}/i2s/i2s_in.pio
)
target_include_directories(passthrough_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(passthrough_test
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_irq
)
pico_add_extra_outputs(passthrough_test)
pico_enable_stdio_usb(passthrough_test 0)
pico_enable_stdio_uart(passthrough_test 1)

# ============================================================
# PRODUCT — pico_tone_trixter: guitar → ES8388 ADC → Pico 2 dual-core 2048-tap
# IR convolution → DSP chain (input trim / 3-band EQ / comp-limiter-sustain /
# output) → ES8388 DAC. Plus a YIN tuner and runtime-switchable presets, all
# live-tunable over UART. Built from main.cpp (promoted from the former
# `es8388_test` in the 2026-06-09 pipeline refactor).
# ============================================================
add_executable(pico_tone_trixter
    main.cpp
    audio/es8388.cpp
    audio/dsp_chain.cpp
    audio/tuner.cpp
    i2s/i2s.c
)
pico_generate_pio_header(pico_tone_trixter
    ${CMAKE_CURRENT_LIST_DIR}/i2s/i2s_out.pio
)
pico_generate_pio_header(pico_tone_trixter
    ${CMAKE_CURRENT_LIST_DIR}/i2s/i2s_in_slave.pio
)
target_include_directories(pico_tone_trixter PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(pico_tone_trixter
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_irq
    hardware_i2c
    hardware_resets
    hardware_pwm
    fft_convolver
)
# Both IR headers (ir_array_{tanglewood,garrison}.h) are generated from WAV by
# gen_audio_arrays.py and #include'd (each in its own namespace) in main.cpp;
# presets switch between them at runtime. audio_arrays keeps them fresh from the WAVs.
add_dependencies(pico_tone_trixter audio_arrays)
# Copy the whole binary to SRAM at boot — eliminates XIP flash latency on Core 1's
# tail FFT (without it, flash contention stalls the FFT past its window → Core 0
# waits → dropped audio blocks).
pico_set_binary_type(pico_tone_trixter copy_to_ram)
pico_add_extra_outputs(pico_tone_trixter)
pico_enable_stdio_usb(pico_tone_trixter 0)
pico_enable_stdio_uart(pico_tone_trixter 1)

# ---- es8388_codec_bench: same firmware, ENABLE_IR=0 --------------------------
# Codec bench / A/B reference: pure passthrough — no convolver, no Core 1, no DSP
# chain. Validates ES8388 bring-up + the raw signal chain independent of the IR/DSP
# path. (Was es8388_test_passthrough before the 2026-06-09 pipeline refactor.)
add_executable(es8388_codec_bench
    main.cpp
    audio/es8388.cpp
    i2s/i2s.c
)
target_compile_definitions(es8388_codec_bench PRIVATE ENABLE_IR=0)
pico_generate_pio_header(es8388_codec_bench
    ${CMAKE_CURRENT_LIST_DIR}/i2s/i2s_out.pio
)
pico_generate_pio_header(es8388_codec_bench
    ${CMAKE_CURRENT_LIST_DIR}/i2s/i2s_in_slave.pio
)
target_include_directories(es8388_codec_bench PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(es8388_codec_bench
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_irq
    hardware_i2c
    hardware_resets
    hardware_pwm
    fft_convolver
)
add_dependencies(es8388_codec_bench audio_arrays)
pico_add_extra_outputs(es8388_codec_bench)
pico_enable_stdio_usb(es8388_codec_bench 0)
pico_enable_stdio_uart(es8388_codec_bench 1)