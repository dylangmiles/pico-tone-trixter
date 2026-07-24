/*
 * es8388.cpp — ES8388 codec I2C initialisation
 *
 * Two-phase init to avoid I2C/SCLK crosstalk:
 *   1. es8388_config_only()    — all config registers, call with SCLK quiesced (GPIO LOW)
 *   2. es8388_chippower_cycle()— CHIPPOWER reset, call with SCLK quiesced (GPIO LOW)
 *   3. es8388_adcpower_resync()— ADCPOWER 0xFF→0x00 sync trigger, call with SCLK running
 *
 * es8388_powerup() combines steps 2+3 and requires SCLK running throughout.
 * The caller (es8388_test.cpp) drives GPIO 16/17 LOW via SIO during quiesced phases
 * so there is zero interference on SDA/SCL.
 */

#include "es8388.h"
#include "pico/stdlib.h"
#include <cstdio>

bool es8388_write_reg(i2c_inst_t *i2c, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    for (int attempt = 0; attempt < 10; attempt++) {
        int result = i2c_write_timeout_us(i2c, ES8388_ADDR, buf, 2, false, 100000);
        if (result == 2) return true;
        sleep_ms(1);
    }
    printf("ES8388 I2C write FAILED reg=0x%02X val=0x%02X\n", reg, val);
    return false;
}

static bool es8388_write(i2c_inst_t *i2c, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    for (int attempt = 0; attempt < 10; attempt++) {
        int result = i2c_write_timeout_us(i2c, ES8388_ADDR, buf, 2, false, 100000);
        if (result == 2) return true;
        sleep_ms(1);
    }
    printf("ES8388 I2C write FAILED reg=0x%02X val=0x%02X\n", reg, val);
    return false;
}

static bool es8388_write_verify(i2c_inst_t *i2c, uint8_t reg, uint8_t val) {
    for (int attempt = 0; attempt < 10; attempt++) {
        if (!es8388_write(i2c, reg, val)) continue;
        uint8_t rb = 0xFF;
        if (es8388_read(i2c, reg, &rb) && rb == val) return true;
        printf("ES8388 verify MISMATCH reg=0x%02X wrote=0x%02X read=0x%02X\n", reg, val, rb);
        sleep_ms(2);
    }
    printf("ES8388 verify FAILED reg=0x%02X val=0x%02X\n", reg, val);
    return false;
}

bool es8388_read(i2c_inst_t *i2c, uint8_t reg, uint8_t *val) {
    for (int attempt = 0; attempt < 10; attempt++) {
        if (i2c_write_timeout_us(i2c, ES8388_ADDR, &reg, 1, true, 100000) == 1) {
            if (i2c_read_timeout_us(i2c, ES8388_ADDR, val, 1, false, 100000) == 1) return true;
        }
        sleep_ms(1);
    }
    return false;
}

bool es8388_config_only(i2c_inst_t *i2c) {
    bool ok = true;
    // CONTROL1=0x12: Play+Record mode (enables ADC digital output path).
    // Without this the chip defaults to DAC-only mode and ADC DOUT stays digital zero.
    ok &= es8388_write(i2c, 0x00, 0x12);  // CONTROL1: Play+Record mode (validated working baseline; comment about bit 7 is misleading — the value 0x12 is what the chip wants here on this part).
    printf("ES8388 config: DAC\n");
    ok &= es8388_write(i2c, 0x19, 0x04);  // DACCONTROL3: mute
    ok &= es8388_write(i2c, 0x01, 0x50);  // CONTROL2: refs on
    ok &= es8388_write(i2c, 0x08, 0x00);  // MASTERMODE: slave
    ok &= es8388_write(i2c, 0x17, 0x20);  // DACCONTROL1: 32-bit WL (bits 5:3=100) + Philips I²S (bits 2:1=00). Was 0x18 (16-bit Philips, validated end-to-end including chain-gain/latency tests on 2026-04-27). Stepping up to 32-bit now that the chip is bit-aligned and the chain is clean. PIO timing unchanged (32 BCLKs per channel slot; chip just fills all 32 with audio instead of 16 + 16 zeros).
    ok &= es8388_write(i2c, 0x18, 0x02);  // DACCONTROL2: single speed, DACFsRatio=00010 (MCLK/LRCK=256 → 48 kHz at 12.288 MHz MCLK). Reverted from 0x20 (double-speed 96 kHz) 2026-06-07 — 96k ran the IR convolver 2x over budget (dropped blocks) and played the 48k IR at the wrong rate. Must match I2S_SAMPLE_RATE in i2s.h.
    ok &= es8388_write(i2c, 0x1A, 0x00);  // DACCONTROL4: LDAC 0 dB
    ok &= es8388_write(i2c, 0x1B, 0x00);  // DACCONTROL5: RDAC 0 dB
    ok &= es8388_write(i2c, 0x26, 0x00);  // DACCONTROL16: DAC audio path, no LIN bypass
    ok &= es8388_write(i2c, 0x27, 0x90);  // DACCONTROL17: left DAC → left mixer, 0dB
    ok &= es8388_write(i2c, 0x2A, 0x90);  // DACCONTROL20: right DAC → right mixer, 0dB
    ok &= es8388_write(i2c, 0x2B, 0x80);  // DACCONTROL21: ADC & DAC share LRCK (required for ADC I2S output)
    ok &= es8388_write(i2c, 0x2D, 0x00);  // DACCONTROL23: vroi=0
    ok &= es8388_write(i2c, 0x04, 0x3C);  // DACPOWER: all four output enables (bits 5,4,3,2); bits 7,6 are DAC power-down so keep at 0
    // Output driver volumes default to 0x00 = -45 dB (near-mute). Set all four to 0x1E (0 dB).
    ok &= es8388_write(i2c, 0x2E, 0x1E);  // LOUT1VOL: 0 dB (default 0x00 = -45 dB)
    ok &= es8388_write(i2c, 0x2F, 0x1E);  // ROUT1VOL: 0 dB
    ok &= es8388_write(i2c, 0x30, 0x1E);  // LOUT2VOL: 0 dB
    ok &= es8388_write(i2c, 0x31, 0x1E);  // ROUT2VOL: 0 dB
    ok &= es8388_write(i2c, 0x19, 0x02);  // DACCONTROL3: unmute, VOLSAME=1
    printf("ES8388 config: ADC\n");
    ok &= es8388_write(i2c, 0x03, 0xFF);  // ADCPOWER: off while configuring
    // ADCCONTROL1 (reg 0x09): bits[7:4]=MicAmpL, bits[3:0]=MicAmpR.
    // Each nibble: 0=0dB, 1=+3, 2=+6, 3=+9, ... 8=+24dB.
    // 2026-05-10 (third iteration): 0x77 (+21 dB) was too hot in combination
    // with digital ×4 in input_dma_irq1_handler — total +33 dB chain pushed
    // open-board pickup noise (12 mV at TSin idle) close to digital FS,
    // triggering codec noise-shaping artefacts at ~19 kHz (118 mV LOUT noise
    // floor at rest, audible as loud HF squeal + harsh clipping during strum).
    // 0x66 (+18 dB) was the JFET-daughter value: a middle ground between the
    // first iteration (+18 dB-only, signal lost in noise) and the over-driven
    // second (+33 dB, codec noise-shaping artefacts).
    // 2026-07-11: analog front end switched to the OPA1642 op-amp daughter
    // (+6 dB gain, analog_input_opamp_buffer_2026-07-09.md). That +6 dB now sits
    // AHEAD of the ADC, so the PGA drops +18→+12 dB (0x66→0x44) to restore ADC
    // headroom — at +18 the op-amp clipped the ADC ~6 dB early (bench 2026-07-11,
    // opamp_vs_jfet_snr_2026-07-11). Net: same total chain gain / output level,
    // better SNR (gain moved forward — Friis). The digital trim (dsp_chain
    // in.level) is unchanged; only ADC-headroom gain moves.
    // 2026-07-24: **0x00 (0 dB) is now the permanent default, for every daughter.** Two
    // measurements ended the search for the "right" PGA value:
    //   - Test C: the ADC clips at 1.35 V pk at LIN2, and the ceiling scales 10^(-pga/20). At the
    //     old 0x44 the K&K's ceiling was 0.16 V, so every body tap and hard strum clipped the
    //     converter. `pga 0` is the only setting that clears normal playing.
    //   - SNR session (Block 3): +18 dB of PGA raised the noise floor +17.4 dB — the noise is
    //     ahead of the PGA, so gain here moves signal and noise together and buys no SNR at all.
    // Together: `pga 0` costs nothing and buys back all the headroom. Make up level DIGITALLY
    // after the ADC (dsp_chain in.level, whose range was widened to 8.0 to absorb it). Do not
    // "restore" a nonzero value here to make a preset louder — that trades headroom for nothing.
    ok &= es8388_write(i2c, 0x09, 0x00);  // 0 dB — headroom-first staging (was 0x44 op-amp / 0x66 JFET)
    // ADCCONTROL2 input select: 0x00=LIN1/RIN1  0x50=LIN2/RIN2
    ok &= es8388_write(i2c, 0x0A, 0x50);  // ADCCONTROL2: LIN2/RIN2
    ok &= es8388_write(i2c, 0x0B, 0x02);  // ADCCONTROL3: stereo
    // ADCCONTROL4: 32-bit Philips. Layout (per ES8388 user guide reg 12):
    // [7:6]=DATSEL=00 (channels independent), [5]=ADCLRP=0 (normal polarity),
    // [4:2]=ADCWL=100 (32-bit), [1:0]=ADCFORMAT=00 (I2S/Philips). Was 0x0C
    // (16-bit Philips) — moving to 32-bit end-to-end now that chain is validated.
    ok &= es8388_write_verify(i2c, 0x0C, 0x10);

    ok &= es8388_write(i2c, 0x0D, 0x02);  // ADCCONTROL5: single speed, ADCFsRatio=00010 (MCLK/LRCK=256 → 48 kHz). Reverted from 0x20 (double-speed 96 kHz) 2026-06-07. Mirrors DACCONTROL2 (0x18).
    ok &= es8388_write(i2c, 0x0F, 0x00);  // ADCCONTROL7: L ADC 0 dB
    ok &= es8388_write(i2c, 0x10, 0x00);  // ADCCONTROL8: R ADC 0 dB
    ok &= es8388_write(i2c, 0x11, 0x00);  // ADCCONTROL9: ALC/volume init (btstack initialises this)
    ok &= es8388_write(i2c, 0x12, 0x00);  // ADCCONTROL10: ALC OFF (ALCSEL=00 disables ALC on both channels)
    ok &= es8388_write(i2c, 0x13, 0x00);  // ADCCONTROL11: ALC hold/decay time (default)
    ok &= es8388_write(i2c, 0x16, 0x00);  // ADCCONTROL14: noise gate off
    // ADCPOWER stays 0xFF here — adcpower_resync() writes 0x09 with SCLK running
    printf("ES8388 config: %s\n", ok ? "OK" : "FAILED");
    return ok;
}

// Call with SCLK quiesced (GPIO LOW). Writes CHIPPOWER and MASTERMODE only.
// ADCPOWER is NOT written here — caller must write 0xFF (quiesced) before calling,
// and then call adcpower_resync() with SCLK running to write 0x09 as the sync trigger.
// 0xF0 pulse: analog-only power-down, preserves digital config (matches reference driver).
bool es8388_chippower_cycle(i2c_inst_t *i2c) {
    bool ok = es8388_write(i2c, 0x02, 0xFF);  // CHIPPOWER: full digital+analog reset (clears stuck I2S SM)
    sleep_ms(10);
    ok &= es8388_write(i2c, 0x02, 0x00);       // CHIPPOWER: full power up
    sleep_ms(50);                               // VMID recharge
    ok &= es8388_write(i2c, 0x08, 0x00);       // MASTERMODE: slave (re-latch)
    return ok;
}

// Call with SCLK running. Writes ADCPOWER=0x09 (ADC on, MICBIAS off, INT1LP).
// ADCPOWER must be 0xFF before this is called (set during quiesced phase).
// The 0xFF→0x09 transition with live SCLK is the I2S ADC sync trigger.
bool es8388_adcpower_resync(i2c_inst_t *i2c) {
    bool ok = es8388_write(i2c, 0x08, 0x00);   // MASTERMODE: slave (re-latch)
    ok &= es8388_write(i2c, 0x03, 0x00);        // ADCPOWER: 0xFF→0x00 sync trigger (ADC fully on)
    return ok;
}

// Call with SCLK already running. Combines CHIPPOWER=0x00 + adcpower_resync.
bool es8388_powerup(i2c_inst_t *i2c) {
    bool ok = es8388_write(i2c, 0x02, 0x00);   // CHIPPOWER: power up (SCLK running)
    sleep_ms(50);                               // VMID recharge
    return ok & es8388_adcpower_resync(i2c);
}

bool es8388_init(i2c_inst_t *i2c) {
    if (!es8388_config_only(i2c)) return false;
    return es8388_powerup(i2c);
}

bool es8388_adc_restart(i2c_inst_t *i2c) {
    bool ok = es8388_write(i2c, 0x08, 0x00);
    ok &= es8388_write(i2c, 0x03, 0x00);
    return ok;
}

// Legacy wrapper — prefer calling chippower_cycle (SCLK quiesced) then adcpower_resync (SCLK running).
bool es8388_chippower_resync(i2c_inst_t *i2c) {
    if (!es8388_chippower_cycle(i2c)) return false;
    return es8388_adcpower_resync(i2c);
}