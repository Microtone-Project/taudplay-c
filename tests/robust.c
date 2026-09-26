/* Robustness: open and render thousands of randomly corrupted variants of a
 * file. Build with sanitizers (make robust) — nothing may crash or trip UB. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "taudplay.h"

static uint32_t rng = 1;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void cb(int n, unsigned arg, void *u) { (void)n; (void)arg; (void)u; }

int main(int argc, char **argv)
{
    FILE *f;
    long sz;
    unsigned char *orig, *buf;
    int iters = argc > 2 ? atoi(argv[2]) : 500, i, opened = 0;
    long chunks = 0;
    if (argc < 2 || (f = fopen(argv[1], "rb")) == NULL) { fprintf(stderr, "usage: robust FILE [ITERS] [SEED]\n"); return 2; }
    if (argc > 3) rng = (uint32_t)strtoul(argv[3], NULL, 0) | 1;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    orig = malloc((size_t)sz); buf = malloc((size_t)sz);
    if (fread(orig, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    for (i = 0; i < iters; i++) {
        TaudConfig cfg;
        TaudPlayer *p;
        int k, nmut = 1 + (int)(next() % 24), n;
        size_t len = (size_t)sz;
        memcpy(buf, orig, (size_t)sz);
        for (k = 0; k < nmut; k++) {
            size_t at, tail = (size_t)sz > 300000 ? 300000 : (size_t)sz;
            switch (next() % 4) {
            case 0: at = next() % (size_t)sz; break;
            case 1: at = next() % 64; break;
            default: at = (size_t)sz - 1 - next() % tail; break;
            }
            if (at >= (size_t)sz) at = (size_t)sz - 1;
            switch (next() % 4) {
            case 0: buf[at] ^= (unsigned char)(1u << (next() % 8)); break;
            case 1: buf[at] = (unsigned char)next(); break;
            case 2: buf[at] = (unsigned char)((next() & 1) ? 0xff : 0x00); break;
            default: if (next() % 64 == 0) len = at; else buf[at] = (unsigned char)(buf[at] + 1); break;
            }
        }
        taud_config_init(&cfg);
        cfg.binaural = (int)(next() & 1);
        cfg.sample_rate = (next() % 3 == 0) ? 44100 : 48000;
        p = taud_open(buf, len, &cfg, NULL);
        if (p == NULL) continue;
        opened++;
        for (n = 0; n < TAUD_NUM_INTERRUPTS; n++) taud_set_interrupt(p, n, cb, NULL);
        for (n = 0; n < taud_song_count(p) && n < 2; n++) {
            int c;
            if (taud_select_song(p, n) != TAUD_OK) break;
            taud_play(p);
            taud_set_voice_gain(p, (int)(next() % 64), 0.5, 0.1);
            for (c = 0; c < 400 && taud_render_chunk(p) != NULL; c++, chunks++) {
                if (c % 97 == 0) { (void)taud_get_voice_volume(p, c % 64); (void)taud_get_voice_pan(p, c % 64); }
            }
        }
        taud_close(p);
    }
    printf("robust: %d iterations, %d opened, %ld blocks rendered\n", iters, opened, chunks);
    free(orig); free(buf);
    return 0;
}
