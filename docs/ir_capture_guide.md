# IR Capture Guide — Tone Trixter

How to capture an acoustic body impulse response (IR) that transforms a piezo pickup signal into a microphone-quality recording.

This is the method used to capture the IRs included in this project. The process is straightforward — the barrier to a high-quality result is the quality of your room, microphone, and playing, not the tooling.

---

## The Concept

A piezo pickup and a microphone placed in front of the same guitar capture the same instrument in fundamentally different ways. The microphone hears the full acoustic resonance of the guitar body. The piezo hears a mechanical signal from the saddle — brighter, thinner, with less warmth and body.

The difference between the two signals *is* the IR. By recording both simultaneously, we can calculate the transfer function that maps piezo → microphone. Apply that IR to any future piezo recording and it sounds like the microphone.

```
Piezo signal  ──┐
                ├──[deconvolution]──► IR (impulse response)
Mic signal    ──┘

Future use:
Piezo signal  ──[IR convolution]──► Sounds like mic
```

This is a transfer function approach, not a room IR. It captures the acoustic character of the guitar body and the microphone's response — not room reverb.

---

## What You Need

### Hardware
- **Guitar** with piezo pickup (undersaddle or soundboard transducer)
- **Microphone** — any decent condenser. An NT1-A or similar large-diaphragm condenser works well. Better mic = better IR.
- **Audio interface** — needs at least 2 simultaneous input channels with no DSP or effects on the signal path. The UA Gigcaster 8 works well: clean preamps, 2+ channels, no processing applied.
- **DAW** — any DAW that can record two channels simultaneously. Luna, Logic, Reaper, etc.
- **Stands** — mic stand to position the microphone consistently

### Software (free)
- [Cuki IR Generator](https://github.com/kienphanhuy/Cuki-IR-generator-Python) — Python/Colab tool that performs the deconvolution
- [Nembrini IR Loader](https://www.nembriniaudio.com/products/ir-loader-impulse-response) or any DAW IR plugin — for auditioning the result before moving to hardware

---

## Signal Chain

```
Guitar
  ├── Piezo output ──► Audio interface channel 1 (DI, no effects)
  └── Mic (in front of guitar) ──► Audio interface channel 2 (clean preamp, no EQ/compression)

Audio interface ──► DAW ──► Stereo WAV (L = piezo, R = mic)
```

**Critical:** No effects, EQ, compression, or limiting on either channel. The raw signal is what the algorithm needs.

---

## Recording

1. **Position the microphone** — 20–30cm from the soundhole, angled slightly toward the upper bout. Experiment: the mic position affects the tonal character of the IR.

2. **Set levels** — aim for peaks around –12dBFS. Leave headroom. Clip on either channel ruins the IR.

3. **Play for 3–4 minutes** — cover the full neck: open chords, fingerpicking, strumming, notes up the neck on every string. The algorithm needs a representative sample of the guitar's full frequency response. Don't play the same thing repeatedly.

4. **Export as stereo WAV**:
   - Channel 1 (Left): piezo
   - Channel 2 (Right): microphone
   - Sample rate: **48000 Hz** (matches Tone Trixter firmware)
   - Bit depth: 24-bit or 32-bit float

---

## Generating the IR

Use the [Cuki IR Generator Colab](https://github.com/kienphanhuy/Cuki-IR-generator-Python):

1. Upload your stereo WAV (L = piezo, R = mic)
2. Run the notebook
3. Three IR files are output:
   - `IR_XXXX_48k_2048_Std.wav` — standard algorithm
   - `IR_XXXX_48k_2048_Std_Bld.wav` — 50/50 blend of raw piezo and IR
   - `IR_XXXX_48k_2048_M.wav` — modified algorithm (usually cleaner result)

The `_M` variant is typically the best starting point. The filename encodes the sample rate and IR length: `2048` = 2048 samples.

---

## Auditioning in the DAW

Before moving to hardware, validate the IR in your DAW:

1. Load the dry piezo recording on a track
2. Apply the IR using an IR loader plugin — [Nembrini IR Loader](https://www.nembriniaudio.com/products/ir-loader-impulse-response) works well
3. A/B between dry piezo and IR-processed signal
4. Compare to the original mic recording — they should sound closely matched

If the result has:
- **Too much high-frequency harshness** — the mic position was too close to the soundhole
- **Resonance peaks or ringing** — try the `_Std` variant instead of `_M`
- **Thin low end** — mic may have been too far off-axis

Adjust mic position and re-record if needed.

---

## Validating on the Pico

Once you're happy in the DAW, use the Tone Trixter offline test pipeline:

1. Place the IR WAV in `audio/samples/` following the naming convention:
   `IR_<guitar>-<mic>-<date>_<samplerate>k_<length>_M.wav`
   Example: `IR_garrison-NT1-A-20260320_48k_2048_M.wav`

2. Rebuild the firmware — CMake regenerates `ir_array.h` and `piezo_raw.bin` automatically

3. Flash `offline_test` and capture output with `tools/capture_wav.py`

4. Run `tools/validate_ir.py` to confirm the Pico output matches the Python reference convolution within tolerance

---

## Quality Considerations

**The methodology is not the barrier.** The process above is straightforward and freely available. What separates a good IR library from a mediocre one:

| Factor | Notes |
|---|---|
| **Room acoustics** | Parallel walls cause comb filtering. A treated room or a dead room (lots of soft furnishings) gives cleaner results. |
| **Microphone quality** | An NT1-A is a solid starting point. A matched pair of Neumann KM184s or a Coles 4038 ribbon gives noticeably better results. |
| **Microphone placement** | Takes experimentation per guitar. Document the position for each capture. |
| **Guitar setup** | Fresh strings, properly intonated, no fret buzz. The IR captures the instrument as it is. |
| **Playing technique** | Consistent dynamics, even tone across the neck. The algorithm averages over the whole recording. |
| **Multiple positions** | Capturing 3–5 mic positions per guitar and curating the best gives buyers options. |

---

## IR Specifications Used in This Project

| Parameter | Value | Reason |
|---|---|---|
| Sample rate | 48000 Hz | Matches Tone Trixter firmware `I2S_SAMPLE_RATE` |
| IR length | 2048 samples | 42.7ms — captures body resonance without excessive CPU cost |
| Format | 24-bit mono WAV | Sufficient dynamic range; mono = single mic position |
| Algorithm | `_M` variant | Cleaner deconvolution result in testing |

The 2048-sample length was chosen after benchmarking on RP2350: Core 0 runs at 2.24× headroom, Core 1 at 7.1× headroom — comfortable margin for real-time processing.