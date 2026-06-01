# Social Media Copy — Episode 1

---

## YouTube Community Post

Images (gallery order):
1. pico-2-breadboard-with-debug-probe.jpg
2. perf_comparison.png
3. ir_validation-20260401.png

Link: direct to article in GitHub repo (not repo root)

---

Can a $7 chip sound like a $300 microphone?

Piezo pickups are everywhere in acoustic guitars — cheap, reliable, and honestly not great. They have a harsh, nasal "quack" that no amount of EQ quite fixes. A studio microphone on the same guitar sounds completely different: warm, full, alive.

The difference is physics. The microphone hears the whole guitar. The piezo hears the saddle.

I've been building a guitar pedal that fixes this in real-time, using a Raspberry Pi Pico 2 (RP2350) — a chip that costs about R95.

The approach is impulse response convolution: record the guitar simultaneously through the piezo and a condenser mic, calculate the transfer function between the two signals, then apply it to the live piezo signal in real-time. Done right, the piezo starts to sound like the mic was in the room.

The algorithm itself wasn't the hard part. The hard part was making it fast enough.

With a 2048-sample IR — long enough to capture the full resonance of the guitar body — the original Pico (RP2040) was running at 136% of real-time budget. 36% over what the hardware could handle. Not usable.

The Pico 2 (RP2350) has a hardware floating-point unit on a Cortex-M33. The same algorithm that couldn't keep up now runs with 7× headroom on the tail convolution. The chart above shows the difference.

The DSP pipeline is now validated. The next step is wiring up live hardware — ADC, preamp, DAC — to get real audio flowing through it.

Components are on the way. More soon.

Full write-up and code: https://github.com/dylangmiles/pico-tone-trixter

---

## X Thread

Tweet 1/5:
Can a $7 chip sound like a $300 microphone?

I've been building a guitar pedal that fixes piezo pickup "quack" in real-time using a Raspberry Pi Pico 2. Here's where it stands. 🧵

Tweet 2/5:
Piezo pickups have a harsh, nasal tone that EQ can't fix. A studio mic on the same guitar sounds completely different.

The fix: impulse response convolution. Record both signals, compute the transfer function, apply it live. The piezo starts sounding like the mic.

Tweet 3/5:
The problem: a 2048-sample IR (needed to capture the full body resonance) ran at 136% of real-time budget on the RP2040. 36% over. Not usable.

The Pico 2 (RP2350) has a hardware FPU. Same algorithm, same IR — now running with 7× headroom.

~24× speedup on float DSP from one chip swap.

Tweet 4/5:
To validate before wiring up live hardware, I ran an offline test: embedded 7.9 seconds of piezo audio in the firmware, processed it on the Pico, streamed the result back over UART.

Six automated checks: output length, signal modification, spectral shape, numerical accuracy vs Python reference. All pass.

Tweet 5/5:
DSP pipeline is proven. Next step: live signal chain — ADC, preamp, DAC, Pico 2.

Components are on their way.

Code + write-up: https://github.com/dylangmiles/pico-tone-trixter

Built in Cape Town.

---

## Instagram Caption

Can a $7 chip sound like a $300 microphone?

Piezo pickups have a characteristic harshness — "quack" — that no EQ fixes. A condenser mic on the same guitar sounds completely different.

I've been building a guitar pedal that fixes this in real-time using a Raspberry Pi Pico 2 (RP2350). The approach: compute the transfer function between a piezo and a mic recording of the same guitar, then apply it live via FFT convolution.

The original Pico couldn't keep up — 36% over real-time budget. The Pico 2's hardware FPU changed that. Same algorithm, 7× headroom.

DSP pipeline validated. Live hardware next.

Full write-up linked in bio.

#diy #guitar #electronicmusic #embedded #raspberrypipico #dsp #audioengineering #capetown #makersofinstagram #guitarpedal