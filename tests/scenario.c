/* The API scenario of tests/scenario_ref.mjs, through the C API.
 *   scenario FILE RATE BINAURAL OUTPREFIX U8(0|1) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "taudplay.h"

static FILE *logf, *audiof;
static int block, useU8;
static TaudPlayer *r;

static void hexd(double x) { unsigned long long u; memcpy(&u, &x, 8); fprintf(logf, " %016llx", u); }
static void on_int(int n, unsigned arg, void *u) { (void)u; fprintf(logf, "int %d %u @%d\n", n, arg, block); }
static void probes(const char *tag)
{
    int v;
    fprintf(logf, "%s %d %d %d %d %d %d", tag, taud_is_playing(r) ? 1 : 0, taud_cue(r), taud_row(r), taud_bpm(r),
            taud_speed(r), taud_channel_count(r));
    for (v = 0; v < 64; v++) { hexd(taud_get_voice_volume(r, v)); hexd(taud_get_voice_pan(r, v)); hexd(taud_get_voice_gain(r, v)); }
    fprintf(logf, "\n");
}
static void render(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const void *b;
        size_t sz;
        if (useU8) { b = taud_render_chunk_u8(r); sz = TAUD_CHUNK * 2; }
        else { b = taud_render_chunk(r); sz = TAUD_CHUNK * 2 * sizeof(float); }
        block++;
        if (b == NULL) { fprintf(logf, "null @%d\n", block); return; }
        fwrite(b, 1, sz, audiof);
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    long sz;
    unsigned char *bytes;
    TaudConfig cfg;
    char path[1024];
    int n;
    if (argc < 6) return 2;
    f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    bytes = malloc((size_t)sz);
    if (fread(bytes, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    taud_config_init(&cfg);
    cfg.sample_rate = atoi(argv[2]);
    cfg.binaural = atoi(argv[3]);
    useU8 = atoi(argv[5]);
    r = taud_open(bytes, (size_t)sz, &cfg, NULL);
    if (r == NULL) return 1;
    snprintf(path, sizeof path, "%s.log", argv[4]); logf = fopen(path, "w");
    snprintf(path, sizeof path, "%s.%s", argv[4], useU8 ? "u8" : "f32"); audiof = fopen(path, "wb");
    for (n = 0; n < 16; n++) taud_set_interrupt(r, n, on_int, NULL);
    taud_play(r);
    render(100); probes("a");
    taud_set_voice_gain(r, 1, 0.25, 0.5);
    taud_set_voice_gain(r, 3, 0.0, 0);
    render(150); probes("b");
    taud_set_voice_gain(r, 1, 1.0, 1.0);
    taud_set_volume(r, 0.7);
    render(150); probes("c");
    taud_seek_cue(r, 1);
    render(200); probes("d");
    if (taud_song_count(r) > 1) {
        taud_select_song(r, 1); taud_play(r); render(200); probes("e"); taud_select_song(r, 0);
    }
    taud_stop(r); render(3); probes("f");
    taud_play(r); render(300); probes("g");
    taud_set_voice_gain(r, 0, 0.0, 2.0);
    render(3000); probes("h");
    fclose(logf);
    fclose(audiof);
    taud_close(r);
    free(bytes);
    return 0;
}
