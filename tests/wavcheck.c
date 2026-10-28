/* WAV encoder check: encode the float signal tests/wav_ref.mjs wrote, clean
 * and gritty, at the engine rate and at 44.1 kHz, and compare with the files
 * the JavaScript library made from it — byte for byte.
 *   tests/wavcheck PREFIX */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "taudplay.h"

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    static const int rates[2] = { 48000, 44100 };
    static const char *names[2] = { "clean", "gritty" };
    char path[1024];
    size_t flen, i;
    unsigned char *f32;
    int r, q, status = 0;
    if (argc < 2) { fprintf(stderr, "usage: wavcheck PREFIX\n"); return 2; }
    snprintf(path, sizeof path, "%s.f32", argv[1]);
    f32 = slurp(path, &flen);
    if (f32 == NULL) { fprintf(stderr, "wavcheck: cannot read %s\n", path); return 2; }
    for (r = 0; r < 2; r++) {
        for (q = 0; q < 2; q++) {
            size_t wlen, rlen;
            unsigned char *ref;
            uint8_t *wav = taud_encode_wav_quality((const float *)f32, flen / 8, 48000, rates[r],
                                                   q ? TAUD_WAV_GRITTY : TAUD_WAV_CLEAN, &wlen);
            snprintf(path, sizeof path, "%s.%s.%d.wav", argv[1], names[q], rates[r]);
            ref = slurp(path, &rlen);
            printf("wav %s %d Hz:", names[q], rates[r]);
            if (wav == NULL || ref == NULL) { printf(" cannot compare\n"); status = 1; }
            else if (wlen != rlen) { printf(" DIFFER in length (%zu vs %zu)\n", wlen, rlen); status = 1; }
            else {
                for (i = 0; i < wlen && wav[i] == ref[i]; i++) {}
                if (i < wlen) { printf(" DIFFER at byte %zu\n", i); status = 1; }
                else printf(" identical (%zu bytes)\n", wlen);
            }
            free(wav);
            free(ref);
        }
    }
    free(f32);
    return status;
}
