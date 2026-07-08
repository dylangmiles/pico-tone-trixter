// audio/tt_store.h — Tone Trixter on-card settings store.
//
// Reads /tonetrix/config.txt and /tonetrix/presets.txt off the mounted SD card and
// turns them into a runtime Preset table + global config. The format is flow-style
// YAML (flat "key: value" lines, inline [a, b, c] lists, --- between presets,
// # comments) — valid YAML, but with no significant indentation so a card edited in
// any text editor can't silently orphan a key.
//
// Fallback model: if the /tonetrix/ folder / files are absent, nothing is parsed and
// the caller keeps the firmware's built-in presets + defaults (see main.cpp boot).
#ifndef TT_STORE_H
#define TT_STORE_H

#include "audio/dsp_chain.h"   // Preset (kept OUTSIDE the extern "C" block below)
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse /tonetrix/config.txt + /tonetrix/presets.txt from the already-mounted card.
// Safe to call with no card / no folder. Returns true if a config value or >=1 valid
// preset was parsed (i.e. the card overrides the built-ins).
bool tt_store_load(void);

// Parsed preset table to hand to dsp_chain_install_presets(). Returns NULL / *n_out=0
// when no presets were parsed (caller keeps the built-in table). The storage is
// static and valid for the program's life.
const Preset *tt_store_presets(int *n_out);

// config.txt: boot preset name ("" if unspecified). gr_meter default, with *was_set
// telling the caller whether config.txt actually specified it.
const char *tt_store_boot_preset(void);
bool        tt_store_gr_meter(bool *was_set);

// Print a one-shot summary of what was loaded (for the `sdcfg` UART command).
void        tt_store_dump(void);

#ifdef __cplusplus
}
#endif

#endif // TT_STORE_H
