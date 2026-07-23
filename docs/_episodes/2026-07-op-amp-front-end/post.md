---
title: "Threading the Needle: The Front End That Fought Back"
subtitle: "I built a fancier buffer certain it would win. Then I measured it."
episode: 4
date: 2026-07-23
published: false   # DRAFT — reframed around the JFET-wins twist; publish after the K&K tune + photos/video
permalink: /episodes/2026-07-op-amp-front-end/
image:
  path: /assets/og/og-episode-3.jpg
  width: 1200
  height: 630
---

Episode 2 closed with a to-do list. Top of it: replace the JFET buffer that conditions the piezo signal with an **op-amp daughter board** — cleaner, quieter, simpler to tune. I was sure it was the upgrade.

This is that swap. It's also the story of measuring it honestly and watching the plan turn inside out.

---

## What a Front End Even Does

A piezo pickup makes a tiny, fussy signal — millivolts, at a very high impedance. Before anything else can touch it, one small circuit has to *catch* it: present a high enough input impedance that it doesn't load the pickup down, add as little noise as possible, and keep enough headroom not to distort on the loud bits. That circuit is the **front end**, and every clever thing downstream — the impulse response, the EQ, the compressor — is only ever as good as what the front end hands it.

The original catcher was a single JFET. The plan was to replace it with something better.

## The Board

![The op-amp daughter board — a FET-input audio op-amp on a small breakout, mounted on a breadboard-style perfboard that plugs into the same 5-pin socket as the JFET.](assets/opamp-daughter.jpg)

Half of an **OPA1642** — a FET-input audio op-amp — wired as a single-5 V-rail preamp with +6 dB of gain. FET input because the K&K piezo needs a ≥1 MΩ high-impedance load. It drops into the exact same socket as the JFET, so the whole thing is a straight A/B swap: pull one daughter, push the other.

## Three Bugs in One Build

First power-up, the output slammed to the rail and stayed there. The fix came from walking the DC voltages across the chip's own pins, one at a time — which turned up two faults stacked on top of each other: a **missing feedback resistor** (no feedback, so the amp rails), and once that was in, a **solder bridge shorting the inverting input to ground** (input pinned low, so the amp rails *again*). Clear both and the output snapped to a clean mid-rail.

Debugging by measuring the nodes instead of guessing at them — the recurring theme of this whole project, and the reason the rest of this episode exists.

## Threading the Needle

Here's the thing I had backwards.

The K&K has a brutal dynamic range. A sustained note is a few millivolts. A knuckle-tap on the guitar body — the kind of percussion players use all the time — spikes to nearly **two volts**. The signal has to pass through a narrow window: above the noise floor so you can hear it, below the converter's clipping ceiling so it doesn't distort. Thread that needle and the pedal sounds like the room. Miss it and you get hiss or crunch.

I'd assumed the buffer set the top of that window. So I measured where the converter actually clips — the real full-scale voltage at its input. It's **1.35 volts.** That's the eye of the needle, and it's set by the chip you're *feeding*, not the one doing the feeding. A hard body-tap at ~2 V doesn't overload the buffer — it overloads the **converter**, and no buffer in front of it changes that number.

That reframed everything. The front end isn't one component doing a job; it's a chain — pickup, buffer, gain stage, converter — and the signal only gets through clean when every link lines up. Ducks in a row. Worse, the op-amp's +6 dB of gain — the thing I'd been so pleased with — *shrinks* the window: it hands the converter a hotter signal, so it clips **sooner**. The gain I'd added was working against me.

## The Same Tap, Two Ways

Here's what that looks like on the bench. A hard tap on the guitar body through each front end — the sort of percussion players do without thinking. Green is the pickup going in; the other trace is what the front end sends on.

![Op-amp output on a hard body-tap, 3.0 V peak-to-peak going in: the trace is slammed flat against both supply rails — 5.24 V peak-to-peak on a 5 V rail. The peaks aren't loud, they're *gone*, chopped off into crackle. That's hard clipping.](assets/opamp-rails-on-tap.png)

![The JFET fed an even *harder* tap — 4.6 V peak-to-peak going in, against the op-amp's 3.0 — and its output still stays bounded at 2.6 V pp, the biggest peak eased down instead of chopped off. More energy in, less chaos out.](assets/jfet-harder-tap.png)

And the loudest thing I could throw at it — a very hard thumb strum — the JFET still holds together, easing the biggest peaks down gently instead of slamming them flat:

![A very hard thumb strum through the JFET: the tallest peaks compress smoothly rather than clipping — soft, musical limiting, the kind you barely notice, not the crackle of a railed op-amp.](assets/jfet-hard-strum.png)

That's the headroom column of the scorecard, in three pictures — and it's the part that surprised me most, because it's the *cheaper* part doing the better job.

## The Op-Amp's Dirty Secret

Then the buffer itself gave something away. Feeding it a clean test tone, the output came back visibly lopsided — nearly **9% distortion**, at small, quiet signal levels where nothing should be straining.

A FET-input op-amp has an input "common-mode range" — a band of voltages its inputs are actually specified to work across. On a single 5 V rail, biased at mid-rail, ours sits about a volt *outside* that band. It works, in the sense that sound comes out. But it's running off-spec, and the distortion is the receipt. The JFET, handed the identical tone: **0.6%.** The fancy part was quietly breaking the rules; the humble one wasn't.

## The Noise Measurement I Owed You

Last episode I promised this one. The bench scope couldn't see either front end's noise floor — both sit below the scope's *own* noise — so I said it was a soundcard-and-FFT job for another day.

This is that day. And the answer settled the whole thing.

![Noise floor, op-amp versus JFET, measured through a soundcard. The JFET's broadband hiss — the lower red trace — sits well below the op-amp's blue across the whole band; its only penalty is the taller spike at 50 Hz on the left, mains hum, which is a grounding problem rather than device noise.](assets/noise-opamp-vs-jfet.png)

Intrinsically, **the JFET is the quieter part** — its broadband hiss runs 8 to 14 dB *below* the op-amp's, right across the audio band. The op-amp's one advantage is less mains hum, and that traces to the JFET's very-high-impedance input picking up more of the 50 Hz in the air — a shielding-and-grounding fix, not a reason to change chips.

And one more result closed the door on my premise for good: turning *up* the codec's own gain buys no quiet at all, because the noise rides straight up with the signal. You don't get a lower floor by amplifying harder before the converter. The only thing that lowers it is a genuinely quieter buffer.

## The Scorecard

Four things decide this front end: **headroom, distortion, hiss, hum.**

| | Headroom | Distortion | Hiss | Mains hum |
|---|:---:|:---:|:---:|:---:|
| **JFET** | ✅ | ✅ | ✅ | — |
| **Op-amp** | — | — | — | ✅ *(fixable)* |

The JFET — the part I set out to *replace* — wins three of the four. The op-amp takes only the hum, and that one's a grounding problem I can design out. I built Plan B certain it was the answer, and the measurements walked me straight back to Plan A, understood far better than when I left it.

## Where We Are

The JFET is the front end. That's not where I expected this to land, and it's exactly why you measure instead of assume — the fancy part isn't automatically the better one, and the ceiling isn't always where you think it is.

Next: re-tune the levels around the JFET for both the passive K&K and a hot active pickup, then rebuild the JFET daughter *properly* — a wider supply rail for margin, a corrected bias for symmetric headroom, and shielding for that mains pickup. Quite possibly on the first real circuit board of the project. Same needle; steadier hands.

---

Source, schematics, and firmware: [github.com/dylangmiles/pico-tone-trixter](https://github.com/dylangmiles/pico-tone-trixter)
