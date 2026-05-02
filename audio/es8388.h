#pragma once
#include "hardware/i2c.h"

#define ES8388_ADDR    0x10
#define ES8388_SDA_PIN 14   /* I²C1 SDA — LEFT side of Pico (GP14 = I²C1 SDA in silicon) */
#define ES8388_SCL_PIN 15   /* I²C1 SCL — LEFT side of Pico (GP15 = I²C1 SCL in silicon) */

bool es8388_read(i2c_inst_t *i2c, uint8_t reg, uint8_t *val);
bool es8388_write_reg(i2c_inst_t *i2c, uint8_t reg, uint8_t val);

// Phase 1: write all config registers. Call with SCLK quiesced (GPIO 27/28 driven LOW).
bool es8388_config_only(i2c_inst_t *i2c);

// Phase 2a: CHIPPOWER 0xFF→0x00 full digital+analog reset + 50ms VMID wait.
// Call with SCLK quiesced — clears stuck I2S state machine after sync loss.
bool es8388_chippower_cycle(i2c_inst_t *i2c);

// Phase 2b: MASTERMODE=0x00 + ADCPOWER 0xFF→0x00 sync trigger (ADC fully on).
// Call with SCLK running — the 0xFF→0x00 transition with live SCLK locks I2S output.
bool es8388_adcpower_resync(i2c_inst_t *i2c);

// Convenience: CHIPPOWER=0x00 (no reset) + adcpower_resync. SCLK must be running.
bool es8388_powerup(i2c_inst_t *i2c);

// Convenience: config_only + powerup (SCLK running throughout — use two-phase in test code).
bool es8388_init(i2c_inst_t *i2c);

// Legacy: chippower_cycle + adcpower_resync in one call. Requires SCLK running for ADCPOWER.
// Prefer calling the two functions separately with SCLK quiesced/resumed between them.
bool es8388_chippower_resync(i2c_inst_t *i2c);

bool es8388_adc_restart(i2c_inst_t *i2c);