// audio/backing.h — SD-streamed backing tracks (drum loops / play-along beds).
//
// Streams a WAV off the card and mixes it into the output AFTER the DSP chain, so
// the backing bed never goes through the IR, EQ or compressor — those exist to
// shape the guitar, and running a drum loop through a cab IR sounds wrong.
//
// Threading: the producer (backing_service) and the consumer (backing_mix) BOTH run
// in the Core 0 foreground loop, sequentially, so there is no cross-core state here
// and no atomics are needed. The cost is that a slow SD read directly delays block
// publication — hence every read is bounded (BACKING_CHUNK_BYTES) and the service
// call is timed. See `bk stat`.
//
// Failure model: if the card cannot keep up, the BACKING drops out and the guitar
// path is untouched. Underruns are counted, never hidden. A backing track is a
// convenience; the instrument signal is not.
#ifndef TT_BACKING_H
#define TT_BACKING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKING_MAX_FILES  16
#define BACKING_NAME_LEN   32

// (Re)scan /tonetrix/backing for *.wav. Safe with no card / no folder (count 0).
// Blocking — call at boot or from a UART/menu action, never mid-block.
void        backing_scan(void);
int         backing_count(void);
const char *backing_name(int i);          // "" when out of range

// Start streaming file i (0..count-1), from the top. Returns 0 on success, or a
// negative BACKING_ERR_*. Blocking (opens + parses the header + primes the ring).
int         backing_play(int i);
void        backing_stop(void);           // stop and close; ring is discarded
bool        backing_playing(void);
int         backing_current(void);        // index playing, or -1

// Producer: top up the ring. Call once per foreground iteration. Bounded work.
void        backing_service(void);

// Consumer: mix `n` samples of backing into dst[] at the current level, resampling
// from the file's rate to the engine rate. Adds; does not overwrite. On underrun it
// adds nothing and bumps the underrun count, leaving the guitar path untouched.
void        backing_mix(float *dst, int n);

void        backing_set_level(float g);   // 0..2, clamped
float       backing_level(void);

// Diagnostics for `bk stat`.
void        backing_stats(uint32_t *underruns, int *ring_pct, uint32_t *max_service_us);
void        backing_stats_reset(void);

// Sequential-read throughput of the card, in kB/s, measured over `kb` kilobytes of
// the currently selected file (or the first file if none is playing). Blocking and
// deliberately disruptive — audio WILL glitch. Answers "can this card stream?".
int         backing_bench(int kb);

const char *backing_err_str(int code);
#define BACKING_ERR_NOFILE  (-1)
#define BACKING_ERR_OPEN    (-2)
#define BACKING_ERR_RIFF    (-3)
#define BACKING_ERR_FMT     (-4)
#define BACKING_ERR_DATA    (-5)
#define BACKING_ERR_UNSUP   (-6)

#ifdef __cplusplus
}
#endif

#endif // TT_BACKING_H
