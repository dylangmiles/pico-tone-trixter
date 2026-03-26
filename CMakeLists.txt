cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico_tone_trixter C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_tone_trixter
    main.c
)

target_link_libraries(pico_tone_trixter
    pico_stdlib
    hardware_pwm
)

pico_add_extra_outputs(pico_tone_trixter)


