// audio/tuner.h — monophonic guitar tuner (YIN pitch detection) for Tone Trixter.
//
// Feed it the dry input blocks; it decimates by 4 (guitar fundamentals are <= ~1.5
// kHz, so 12 kHz is plenty) and runs YIN over an ~85 ms window, producing a note +
// cents estimate a few times a second. Cheap enough to run on Core 0 in a dedicated
// "tuner mode" (IR/chain bypassed). The result struct is display-agnostic so a future
// OLED tuner can render the same data the UART meter shows today.
#ifndef TT_TUNER_H
#define TT_TUNER_H

#include <stdbool.h>

typedef struct {
    float       freq_hz;   // detected fundamental (0 if none)
    float       cents;     // -50..+50 offset from the nearest note
    int         midi;      // nearest MIDI note number
    const char *name;      // note name, e.g. "E", "A#"
    int         octave;    // scientific-pitch octave (MIDI 60 = C4)
    float       clarity;   // 0..1 confidence (1 = very clear)
    bool        valid;     // true if a confident pitch was found
} TunerResult;

void tuner_init(float fs);

// Feed a mono block of n samples. Returns true when a NEW estimate is ready
// (roughly every 85 ms); read it with tuner_result().
bool tuner_feed(const float *samples, int n);

TunerResult tuner_result(void);

#endif // TT_TUNER_H