/* taudplay — command-line front end: list a file's songs, bounce one to WAV
 * or raw float, and print the interrupts it fires.
 *
 *   taudplay song.taud                     describe the file
 *   taudplay song.taud -o out.wav          render song 0 to a 48 kHz WAV
 *   taudplay song.taud -s 2 -t 60 -o x.wav song 2, at most 60 seconds
 *   taudplay song.taud --gritty -o x.wav   the device's own 8-bit sound
 *   taudplay song.taud --raw out.f32       interleaved stereo float32
 *   taudplay song.taud --raw - | ffplay -f f32le -ar 48000 -ac 2 -i pipe:0
 *   taudplay song.taud --interrupts        print the cue sheet the song fires
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "taudplay.h"

static void usage(void)
{
    fprintf(stderr,
        "usage: taudplay FILE.taud [options]\n"
        "  -o FILE.wav        render to a stereo WAV (- = stdout): clean, 16-bit\n"
        "                     with a light TPDF dither\n"
        "  --gritty           make the WAV 8-bit through the device's own\n"
        "                     noise-shaped dither instead\n"
        "  --raw FILE         stream interleaved stereo float32 little-endian at the\n"
        "                     engine rate (- = stdout, e.g. for ffplay -f f32le)\n"
        "  --s16              make --raw signed 16-bit little-endian instead\n"
        "  -s N               song index (default 0)\n"
        "  -t SECONDS         maximum length (default 600)\n"
        "  -r HZ              engine rate (default 48000)\n"
        "  --wav-rate HZ      resample the WAV to this rate (default: engine rate)\n"
        "  --binaural         head-model monitoring for surround songs\n"
        "  --mute V[,V...]    silence these lanes (0-based)\n"
        "  --seed N           seed of the random source (default 1)\n"
        "  --interrupts       print interrupts as they fire\n");
}

static unsigned long long g_frame;
static int g_rate;
static FILE *g_intOut;

/* stdout in binary mode (only Windows distinguishes). */
static void set_binary(FILE *f)
{
#ifdef _WIN32
    _setmode(_fileno(f), _O_BINARY);
#else
    (void)f;
#endif
}

/* Interleaved stereo as little-endian float32 or signed 16-bit, whatever the
 * host's byte order. Returns non-zero when the write fails. */
static int write_raw(FILE *f, const float *block, int frames, int s16)
{
    unsigned char buf[TAUD_CHUNK * 2 * 4];
    int i, n = frames * 2;
    for (i = 0; i < n; i++) {
        if (s16) {
            /* clamp and round exactly as the WAV writer (Math.round) */
            double d = (block[i] < -1.0f ? -1.0 : block[i] > 1.0f ? 1.0 : (double)block[i]) * 32767;
            double r = floor(d);
            unsigned u;
            if (d - r >= 0.5) r += 1.0;
            u = (unsigned)((long)r & 0xffff);
            buf[i * 2] = (unsigned char)u;
            buf[i * 2 + 1] = (unsigned char)(u >> 8);
        } else {
            uint32_t u;
            memcpy(&u, &block[i], 4);
            buf[i * 4] = (unsigned char)u;
            buf[i * 4 + 1] = (unsigned char)(u >> 8);
            buf[i * 4 + 2] = (unsigned char)(u >> 16);
            buf[i * 4 + 3] = (unsigned char)(u >> 24);
        }
    }
    return fwrite(buf, s16 ? 2 : 4, (size_t)n, f) != (size_t)n;
}

static void on_interrupt(int n, unsigned arg, void *user)
{
    (void)user;
    fprintf(g_intOut, "%10.4f s  Int%X  arg %u\n", (double)g_frame / g_rate, n, arg);
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    size_t cap = 0, n = 0;
    if (f == NULL) return NULL;
    for (;;) {
        size_t got;
        if (n == cap) {
            unsigned char *nb;
            cap = cap ? cap * 2 : 1 << 20;
            nb = (unsigned char *)realloc(buf, cap);
            if (nb == NULL) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    fclose(f);
    *len = n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *wavOut = NULL, *rawOut = NULL, *mute = NULL;
    double seconds = 600;
    int wavRate = 0, printInts = 0, s16 = 0, gritty = 0, i;
    TaudConfig cfg;
    TaudError err;
    TaudPlayer *p;
    unsigned char *bytes;
    size_t len;

    taud_config_init(&cfg);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0 && i + 1 < argc) wavOut = argv[++i];
        else if (strcmp(a, "--raw") == 0 && i + 1 < argc) rawOut = argv[++i];
        else if (strcmp(a, "-s") == 0 && i + 1 < argc) cfg.song_index = atoi(argv[++i]);
        else if (strcmp(a, "-t") == 0 && i + 1 < argc) seconds = atof(argv[++i]);
        else if (strcmp(a, "-r") == 0 && i + 1 < argc) cfg.sample_rate = atoi(argv[++i]);
        else if (strcmp(a, "--wav-rate") == 0 && i + 1 < argc) wavRate = atoi(argv[++i]);
        else if (strcmp(a, "--binaural") == 0) cfg.binaural = 1;
        else if (strcmp(a, "--mute") == 0 && i + 1 < argc) mute = argv[++i];
        else if (strcmp(a, "--seed") == 0 && i + 1 < argc) cfg.random_seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(a, "--interrupts") == 0) printInts = 1;
        else if (strcmp(a, "--s16") == 0) s16 = 1;
        else if (strcmp(a, "--gritty") == 0) gritty = 1;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }
        else if (a[0] != '-' && in == NULL) in = a;
        else { usage(); return 2; }
    }
    if (in == NULL) { usage(); return 2; }
    if (rawOut != NULL && wavOut != NULL && strcmp(rawOut, "-") == 0 && strcmp(wavOut, "-") == 0) {
        fprintf(stderr, "taudplay: only one of -o and --raw can write to stdout\n");
        return 2;
    }
    /* When stdout carries audio, the interrupt log moves to stderr. */
    g_intOut = (rawOut != NULL && strcmp(rawOut, "-") == 0) || (wavOut != NULL && strcmp(wavOut, "-") == 0)
        ? stderr : stdout;

    bytes = read_file(in, &len);
    if (bytes == NULL) { fprintf(stderr, "taudplay: cannot read %s\n", in); return 1; }
    p = taud_open(bytes, len, &cfg, &err);
    free(bytes);
    if (p == NULL) { fprintf(stderr, "taudplay: %s: %s\n", in, taud_error_string(err)); return 1; }
    g_rate = taud_sample_rate(p);

    if (wavOut == NULL && rawOut == NULL && !printInts) {
        const char *title = taud_title(p);
        printf("%s\n", title != NULL ? title : "(untitled)");
        for (i = 0; i < taud_song_count(p); i++) {
            TaudSongInfo s;
            taud_song_info(p, i, &s);
            printf("  [%d] %-32s %3d patterns  %3d bpm  %2d lanes%s\n", s.index, s.name, s.patterns,
                   s.bpm, s.channels, s.surround ? "  surround" : "");
        }
        taud_close(p);
        return 0;
    }

    if (mute != NULL) {
        const char *q = mute;
        while (*q) {
            char *end;
            long v = strtol(q, &end, 10);
            if (end == q) break;
            taud_set_voice_gain(p, (int)v, 0.0, 0.0);
            q = *end == ',' ? end + 1 : end;
        }
    }
    if (printInts) {
        int n;
        for (n = 0; n < TAUD_NUM_INTERRUPTS; n++) taud_set_interrupt(p, n, on_interrupt, NULL);
    }

    {
        size_t maxFrames = (size_t)(seconds * g_rate + 0.5), frames = 0, cap = 0;
        float *pcm = NULL;
        const float *block;
        FILE *raw = NULL;
        int status = 0;

        if (rawOut != NULL) {
            if (strcmp(rawOut, "-") == 0) { raw = stdout; set_binary(stdout); }
            else raw = fopen(rawOut, "wb");
            if (raw == NULL) { fprintf(stderr, "taudplay: cannot write %s\n", rawOut); taud_close(p); return 1; }
        }
        taud_play(p);
        while (frames < maxFrames && (block = taud_render_chunk(p)) != NULL) {
            g_frame = frames + TAUD_CHUNK;
            /* Raw output streams as it renders, so a player on the other end
             * of a pipe starts at once rather than after the whole song. */
            if (raw != NULL && write_raw(raw, block, TAUD_CHUNK, s16) != 0) {
                if (raw != stdout) fprintf(stderr, "taudplay: cannot write %s\n", rawOut);
                status = raw == stdout ? 0 : 1;   /* a reader that hangs up is not an error */
                break;
            }
            if (wavOut != NULL) {
                if (frames + TAUD_CHUNK > cap) {
                    float *np;
                    cap = cap ? cap * 2 : (size_t)g_rate * 16;
                    np = (float *)realloc(pcm, cap * 2 * sizeof(float));
                    if (np == NULL) { fprintf(stderr, "taudplay: out of memory\n"); status = 1; break; }
                    pcm = np;
                }
                memcpy(pcm + frames * 2, block, TAUD_CHUNK * 2 * sizeof(float));
            }
            frames += TAUD_CHUNK;
        }
        if (raw != NULL) {
            if (raw == stdout) fflush(stdout); else if (fclose(raw) != 0) status = 1;
        }
        if (wavOut != NULL && status == 0) {
            size_t wlen;
            uint8_t *wav = taud_encode_wav_quality(pcm, frames, g_rate, wavRate > 0 ? wavRate : g_rate,
                                                   gritty ? TAUD_WAV_GRITTY : TAUD_WAV_CLEAN, &wlen);
            int toStdout = strcmp(wavOut, "-") == 0;
            FILE *f = wav == NULL ? NULL : toStdout ? stdout : fopen(wavOut, "wb");
            if (toStdout) set_binary(stdout);
            if (f == NULL || fwrite(wav, 1, wlen, f) != wlen) {
                fprintf(stderr, "taudplay: cannot write %s\n", wavOut);
                status = 1;
            }
            if (f != NULL) { if (toStdout) fflush(f); else fclose(f); }
            free(wav);
        }
        fprintf(stderr, "taudplay: rendered %.2f s%s\n", (double)frames / g_rate,
                taud_is_playing(p) ? "" : " (song halted)");
        free(pcm);
        if (status != 0) { taud_close(p); return status; }
    }
    taud_close(p);
    return 0;
}
