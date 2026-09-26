# taudplay-c

**Play `.taud` songs from C. No tracker, no browser, no dependencies.**

A portable C99 port of [`taudplay`](https://github.com/curioustorvald/microtone-taudplay),
the playback half of the [Microtone](https://microtone.cc) tracker. It is the
same engine, note for note and **sample for sample**: at the reference rate a
render here is bit-identical to the same song rendered by the JavaScript
library (and so by the tracker itself) — verified over the whole Microtone
test corpus, a differential fuzzer and an API-level scenario test (see
[Verification](#verification)).

The surface is the JS library's, kept deliberately small:

| | |
|---|---|
| **Knob** | `taud_set_voice_gain(p, v, gain, fade_seconds)` — one fader per lane |
| **Probe** | `taud_get_voice_volume(p, v)` — how loud lane `v` is, 0…1 |
| **Probe** | `taud_get_voice_pan(p, v)` — where it sits, 0 (left) … 0.5 … 1 (right) |

…plus the transport, and the sixteen **interrupts** a song can fire at the
program playing it (`Int0`…`IntF` in a note column, each with a 16-bit
argument), delivered in time with the music.

## Build

```
make            # libtaudplay.a and the taudplay CLI
```

or CMake (`add_subdirectory` works too):

```
cmake -B build && cmake --build build
```

Any C99 compiler; the only library needed is libm. Two build flags matter:

- **`-ffp-contract=off`** (both build files set it). A fused multiply-add rounds
  once where the reference rounds twice; GCC contracts by default on ARM64 and
  on any target with FMA, and one fused operation is enough to lose bit-exactness.
- **Never `-ffast-math`.** On 32-bit x87 targets also add
  `-fexcess-precision=standard` (or build for SSE2).

Nothing else about the platform is assumed: the engine keeps no global state,
and the transcendental functions come from its own fdlibm port (below), so the
output does not depend on the C library's `sin` or `exp`.

## Play something

```c
#include "taudplay.h"

TaudError err;
TaudPlayer *p = taud_open(file_bytes, file_len, NULL, &err);   /* NULL config = defaults */
if (p == NULL) { puts(taud_error_string(err)); return; }

taud_play(p);
const float *block;
while ((block = taud_render_chunk(p)) != NULL) {
    /* TAUD_CHUNK (128) frames of interleaved stereo float at 48 kHz */
}
taud_close(p);
```

The file bytes are only read during `taud_open`; everything the player needs is
copied out.

### From an audio callback

```c
void audio_callback(void *user, float *out, int frames)
{
    taud_render(player, out, frames);   /* any frame count; silence after the end */
}
```

`taud_render` buffers across block boundaries; `taud_render_s16` does the same
into interleaved `int16_t` (clamped and rounded exactly as the WAV writer does).
`taud_render_chunk_u8` renders the Taud device's own output — noise-shaped,
dithered unsigned 8-bit.

### Render at the device's rate

```c
TaudConfig cfg;
taud_config_init(&cfg);
cfg.sample_rate = 44100;        /* or 32000, 22050, … */
TaudPlayer *p = taud_open(bytes, len, &cfg, NULL);
```

The engine renders natively at any rate — every rate-derived quantity (tick
length, filter and Amiga-LPF coefficients, anti-click ramps, the binaural HRIRs)
is computed from it — so a device running at 44.1 kHz needs no resampler. The
JavaScript library does the same when its engine rate is changed, and the two
still agree bit for bit at any rate; 48 kHz is simply the rate the JS player
uses in a browser.

### Mix it

```c
taud_set_voice_gain(p, 4, 0.0, 1.2);   /* fade lane 5 out over 1.2 s of rendered audio */
taud_set_voice_gain(p, 4, 1.0, 0.4);   /* …and back in, faster */

for (int v = 0; v < taud_channel_count(p); v++)   /* duck everything but the drums */
    if (v != DRUMS) taud_set_voice_gain(p, v, 0.3, 0.8);
```

A fader reaches everything its lane spawned — NNA ghosts and metainstrument
layers included — so a faded lane really does take its whole sound with it.

### Let the song call you

```c
static void on_cue(int n, unsigned arg, void *user) { flash_light(arg); }

taud_set_interrupt(p, 0, on_cue, NULL);   /* NULL fn unregisters */
```

Handlers run on the rendering thread, from inside the render call, once per
128-frame block the interrupt fired in (2.7 ms at 48 kHz), lowest number first.
If one interrupt fires twice inside a block you are called once, with the later
argument. A handler that must not block the audio thread should post the event
somewhere and return.

### Watch it

```c
for (int v = 0; v < taud_channel_count(p); v++)
    draw_meter(v, taud_get_voice_volume(p, v), taud_get_voice_pan(p, v));
```

The probes describe the state at the end of the last rendered block.

## The CLI

```
taudplay song.taud                          describe the file
taudplay song.taud -o song.wav              render song 0 to a 16-bit WAV
taudplay song.taud -s 2 -t 60 -o x.wav      song 2, at most 60 seconds
taudplay song.taud -r 44100 -o x.wav        render natively at 44.1 kHz
taudplay song.taud --wav-rate 44100 -o x.wav  render at 48 kHz, resample the WAV
taudplay song.taud --binaural -o x.wav      head-model downmix of a surround song
taudplay song.taud --mute 0,3 -o x.wav      without lanes 1 and 4
taudplay song.taud --interrupts             print the song's interrupt cue sheet
taudplay song.taud --raw x.f32              interleaved float32, engine rate
taudplay song.taud --raw - | ffplay -f f32le -ar 48000 -ac 2 -i pipe:0
taudplay song.taud --raw - --s16 | aplay -f S16_LE -r 48000 -c 2
taudplay song.taud -o - | ffplay -i pipe:0  WAV to stdout
```

`--raw` streams as it renders (little-endian whatever the host), so a player
on the other end of a pipe starts at once; `-o -` has to render the whole song
first, because a WAV header carries the length. With `-r` the stream is at that
rate, so pass the same `-ar` to ffplay. Messages, and the `--interrupts` log
when stdout carries audio, go to stderr. `-t` still caps the length (600 s by
default).

## API

All in [`include/taudplay.h`](include/taudplay.h).

- **Lifetime:** `taud_config_init`, `taud_open`, `taud_close`, `taud_error_string`
- **The file:** `taud_title`, `taud_song_count`, `taud_song_info`, `taud_select_song`,
  `taud_current_song`, `taud_sample_rate`
- **Transport:** `taud_play` (from the top), `taud_stop`, `taud_seek_cue`,
  `taud_set_volume`, `taud_set_binaural`; `taud_is_playing`, `taud_cue`, `taud_row`,
  `taud_bpm`, `taud_speed`, `taud_channel_count`
- **Knob / probes:** `taud_set_voice_gain`, `taud_get_voice_gain`,
  `taud_get_voice_volume`, `taud_get_voice_pan`
- **Interrupts:** `taud_set_interrupt`, `taud_clear_interrupts`
- **Rendering:** `taud_render_chunk`, `taud_render`, `taud_render_s16`,
  `taud_render_chunk_u8`
- **Utilities:** `taud_encode_wav` (with the library's Kaiser-sinc resampler),
  `taud_gain_to_fader`, `taud_fader_to_gain`

## Embedding notes

- **Threads.** No global state: each `TaudPlayer` is independent and may live on
  its own thread. One player must not be called from two threads at once;
  calling the knobs from a UI thread while the audio thread renders needs your
  own lock (or a command queue).
- **Memory.** The format's sample pool is a flat 8 MB image. It is decompressed
  once at open, then trimmed to its last non-zero byte (a read past the end is
  0, which is what the full pool holds there), so a player keeps about 2 MB of
  its own state plus the song's actual sample data: 2 MB resident for a small
  chiptune, 6 MB for the densest song in the Microtone corpus. Peak memory
  during `taud_open` is about 11 MB, while the full image is decompressed.
- **Allocation on the render path.** None in steady state. The voice pool grows
  (in blocks of 64 voices) only when a note storm outgrows every earlier one,
  and an instrument's invert/modification mask (8 KiB) is allocated the first
  time an effect needs it. Pools are reclaimed by marking what the lanes can
  still reach — the objects the JavaScript engine's garbage collector keeps.
- **Hostile input.** Every length and offset in the container is checked, the
  zstd and inflate decoders are bounds-checked, and the engine is exercised
  under AddressSanitizer + UBSan with thousands of corrupted files
  (`make robust`). Where the JavaScript engine would throw on a malformed file
  (an envelope loop point past its 25 nodes, say) the C engine reads the nearest
  valid entry instead; no file the JavaScript can play is affected by that.
- **Randomness.** Instruments may ask for random volume/pan swing, and some
  effects are randomised. The JS library draws those from `Math.random`; the C
  library from a seeded mulberry32 (`cfg.random_seed`, default 1) or your own
  `cfg.random` — so a render is reproducible, and is exactly the JS render when
  the JS side is seeded the same way (`setRandomSource(makeSeededRandom(1))`).
- **Speed.** About 50× real time on one core of a desktop x86-64 for an ordinary
  32-lane song (`-O2`), ~25× for a dense one full of metainstruments and SF2
  filters.

## Numerics

The engine does its arithmetic in binary64 and narrows the mix to binary32 once
per frame, exactly where the JavaScript does; every expression keeps the
reference's order of operations.

V8 computes `Math.sin`, `cos`, `tan`, `exp`, `log10`, `log2`, `acos`, `asin`
and `atan2` with fdlibm, and the C library's versions differ from it in the last
bit for a few per cent of arguments — enough to move a sample by one binary32
ulp here and there. So [`src/jsmath.c`](src/jsmath.c) carries the fdlibm
functions, verified bit-for-bit against V8 over millions of arguments
(`tests/jsmath_check`). That also makes the output independent of the target's
libm.

`pow` is the exception: V8's agrees with glibc's and musl's (both effectively
correctly rounded), not with fdlibm's, so the platform `pow` is used. On a C
library with a less accurate `pow`, build with `-DTAUD_FDLIBM_POW` (CMake:
`-DTAUDPLAY_FDLIBM_POW=ON`) to get output that is identical on every platform —
inaudibly different from the JS reference, but deterministic everywhere.

## Verification

`make check` (needs Node ≥ 22 and a checkout of the JS library at
`../microtone-taudplay`, or `JS=path`) runs:

- `tests/jsmath_check` — the math functions against V8.
- `tests/run.sh` — every song of every corpus file, stereo fold-down and
  binaural, JS vs C, sample for sample. Also takes any files:
  `sh tests/run.sh path/to/*.taud`.
- `tests/scenario.sh` — the public API against the JS `TaudRenderer`: faders
  and fades, master volume, seeking, song switching, stop and replay, probes,
  interrupts, the dithered 8-bit output, and non-48 kHz engine rates. Audio and
  every probe reading (as raw bits) must match.
- `tests/fuzz.sh` — differential fuzzing: `tests/gen_fuzz.mjs` writes random but
  valid songs (both cell formats, 32/64 lanes, stereo/planar/spatial, envelopes,
  IT/SF2 filters, NNA and duplicate checks, Ixmp patches including stereo ones,
  layered and FM metainstruments, every effect command, mastering chains), and
  both implementations must render them identically.

At the time of writing all of it is bit-identical: the thirteen files of the
Microtone corpus and demos (every song, both monitor modes), and hundreds of
fuzzed songs.

## Relationship to the JS library

This is a port of `src/engine`, `src/format`, `src/audio/offline-render.js`
and `src/taudplay/render.js` — the `TaudRenderer` half. The browser half
(`TaudPlayer`, the AudioWorklet) has no C counterpart: its job, pulling blocks
from an audio callback, is `taud_render`. Source files mirror the JS modules one
for one (`sampler.c` is `sampler.js`, `tick.c` is `tick.js`, …) and keep their
function names, so the two can be diffed when the engine moves.

## Licence

LGPL-3.0-or-later — see `COPYING.LESSER` (and `COPYING` for the GPL text it
builds on). You may link this library into a proprietary program; changes *to
the library itself* must be shared.

`src/jsmath.c` is derived from fdlibm, Copyright © 1993 Sun Microsystems, Inc.
("Permission to use, copy, modify, and distribute this software is freely
granted, provided that this notice is preserved"). The binaural filter set in
`src/hrir_sadie.c` is the GoogleVR/SADIE set, © 2017 Google Inc. and the
University of York, Apache-2.0.

---

Songs are made with **Microtone** — a tracker for the notes a piano cannot
play, free and in your browser at [microtone.cc](https://microtone.cc).
