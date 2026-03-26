#include "pico/stdlib.h"
#include "hardware/pwm.h"

#define AUDIO_PIN 2

void play_tone(uint gpio, uint freq_hz, uint duration_ms) {
    uint slice = pwm_gpio_to_slice_num(gpio);
    uint channel = pwm_gpio_to_channel(gpio);

    gpio_set_function(gpio, GPIO_FUNC_PWM);

    uint32_t clock = 125000000;
    uint32_t divider16 = clock / freq_hz / 4096 + (clock % (freq_hz * 4096) != 0);
    if (divider16 / 16 == 0) divider16 = 16;

    pwm_set_clkdiv_int_frac(slice, divider16 / 16, divider16 & 0xF);
    uint32_t wrap = clock * 16 / divider16 / freq_hz - 1;
    pwm_set_wrap(slice, wrap);
    pwm_set_chan_level(slice, channel, wrap / 2);
    pwm_set_enabled(slice, true);

    sleep_ms(duration_ms);

    pwm_set_enabled(slice, false);
    gpio_set_function(gpio, GPIO_FUNC_SIO);
    gpio_set_dir(gpio, GPIO_OUT);
    gpio_put(gpio, 0);
}

int main() {
    stdio_init_all();
    printf("pico-tone-trixter starting\n");

    while (true) {
        play_tone(AUDIO_PIN, 440, 500);  // A4
        sleep_ms(100);
        play_tone(AUDIO_PIN, 523, 500);  // C5
        sleep_ms(100);
        play_tone(AUDIO_PIN, 659, 500);  // E5
        sleep_ms(500);

        printf("beep\n");
    }

    return 0;
}
