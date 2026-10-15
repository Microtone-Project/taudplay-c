/* Metainstrument type 1 — the FORMANT INSTRUMENT (item 212) — port of formant.js.
 *
 * An ordinary instrument record whose sample is a glottal source, played
 * through Klatt's cascade/parallel formant synthesiser (Klatt 1980), and whose
 * second pitch/filter slot holds what the tract says: a vowel on Catford's
 * cardinal-vowel chart, an onset cluster and a coda (one CCVC syllable, with an
 * optional glide), and how the formants follow pitch. formant.js carries the
 * phonetics; this file keeps its arithmetic, expression for expression.
 *
 * Every transcendental is the JS's own (jsmath.c): exp, log, log2, log10, cos,
 * sin, pow; sqrt is correctly rounded on both sides. */

#include <string.h>

#include "taud_internal.h"

#define PI 3.141592653589793

#define FB_SOURCE 0
#define FB_FRONT 3
#define FB_OPEN 4
#define FB_ROUND 5
#define FB_TRACK 6
#define FB_REF 7
#define FB_GLIDE 9
#define FB_TIMING 10
#define FB_RANGE_LO 11
#define FB_RANGE_HI 14
#define FB_SLOTS 17
#define FB_GR 53
#define FORMANT_SLOT_BYTES 12

static int fu16(const uint8_t *b, int o) { return b[o] | (b[o + 1] << 8); }
static uint32_t fu24(const uint8_t *b, int o) { return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16); }

void te_decode_formant_block(const uint8_t *raw, TaudFormantPatch *patch)
{
    int gr = raw[FB_GR], i;
    patch->srcPtr = fu24(raw, FB_SOURCE);
    patch->front = raw[FB_FRONT];
    patch->open = raw[FB_OPEN];
    patch->round = raw[FB_ROUND];
    patch->track = raw[FB_TRACK];
    patch->refNote = fu16(raw, FB_REF);
    patch->glide = raw[FB_GLIDE];
    patch->timing = raw[FB_TIMING];
    patch->rangeLo = fu24(raw, FB_RANGE_LO);
    patch->rangeHi = fu24(raw, FB_RANGE_HI);
    patch->semivowel = (gr & 7) <= 4 ? gr & 7 : 0;
    patch->rhotic = (gr & 0x80) != 0;
    for (i = 0; i < FORMANT_SLOTS; i++) {
        const uint8_t *o = raw + FB_SLOTS + i * FORMANT_SLOT_BYTES;
        TaudFormantSlot *s = &patch->slots[i];
        int t = o[10];
        s->cons = o[0];
        s->ptr = fu24(o, 1);
        s->len = fu16(o, 4);
        s->offset = fu16(o, 6);
        s->rate = fu16(o, 8);
        s->tilt = t >= 0x80 ? t - 0x100 : t;
        s->gain = o[11];
    }
}

/* ── the vowel ── */

static const int CARDINAL_VOWELS[4][4][2] = {
    { { 240, 2400 }, { 235, 2100 }, { 300, 1390 }, { 250, 595 } },
    { { 390, 2300 }, { 370, 1900 }, { 460, 1310 }, { 360, 640 } },
    { { 610, 1900 }, { 585, 1710 }, { 600, 1170 }, { 500, 700 } },
    { { 850, 1610 }, { 820, 1530 }, { 750, 940 }, { 700, 760 } },
};

static void vowel_f1f2(const TaudEngine *e, double front, double open, double round, double out[2])
{
    const TaudFormantTables *T = &e->formantTables;
    double x = dmin(dmax(front / 255, 0), 1);
    double r = dmin(dmax(round / 255, 0), 1);
    double h = dmin(dmax(open / 255, 0), 1) * 3;
    int i = (int)dmin(2, floor(h)), n;
    double u = h - i;
    double w0 = x * (1 - r), w1 = x * r, w2 = (1 - x) * (1 - r), w3 = (1 - x) * r;
    for (n = 0; n < 2; n++) {
        const double (*lo)[2] = T->logCard[i], (*hi)[2] = T->logCard[i + 1];
        double a0 = lo[0][n] + (hi[0][n] - lo[0][n]) * u;
        double a1 = lo[1][n] + (hi[1][n] - lo[1][n]) * u;
        double a2 = lo[2][n] + (hi[2][n] - lo[2][n]) * u;
        double a3 = lo[3][n] + (hi[3][n] - lo[3][n]) * u;
        out[n] = js_exp(w0 * a0 + w1 * a1 + w2 * a2 + w3 * a3);
    }
}

static double vowel_f1_at(const TaudEngine *e, double front, double open, double round)
{
    double f[2];
    vowel_f1f2(e, front, open, round, f);
    return f[0];
}

#define RHOTIC_F3 1711
#define RHOTIC_F2 1379
#define RHOTIC_F4 3335

static void vowel_formants(const TaudEngine *e, double front, double open, double round, double *F, bool rhotic)
{
    double f12[2], f1, f2, f3u, f3r, f3, f4, f5;
    vowel_f1f2(e, front, open, round, f12);
    f1 = f12[0];
    f2 = f12[1];
    f3u = dmax(2500, f2 + 700);
    f3r = dmax(2200, f2 + 250);
    f3 = f3u * js_pow(f3r / f3u, dmin(dmax(round, 0), 255) / 255);
    if (rhotic) {
        f3 = dmin(f3, RHOTIC_F3);
        f2 = dmin(f2, (f3 * RHOTIC_F2) / RHOTIC_F3);
    }
    f4 = dmax(3500, f3 + 400);
    if (rhotic) f4 = dmin(f4, RHOTIC_F4);
    f5 = dmax(4500, f4 + 500);
    F[0] = f1; F[1] = f2; F[2] = f3; F[3] = f4; F[4] = f5;
}

static double note_hz(int note)
{
    return LINEAR_FREQ_C4_HZ * js_pow(2, (note - MIDDLE_C) / 4096.0);
}

static double vowel_at_pitch(const TaudEngine *e, double front, double open, double round, double f0,
                             double refHz, int track, double *F, double *B, bool rhotic)
{
    double s = f0 > 0 && refHz > 0 ? js_pow(f0 / refHz, track / 256.0) : 1.0;
    double o = open, ceil;
    bool tuned = f0 > vowel_f1_at(e, front, open, round) * s;
    int n;
    if (tuned) {
        double want = f0 / s;
        if (vowel_f1_at(e, front, 255, round) <= want) {
            o = 255;
        } else {
            double a = open, b = 255;
            int i;
            for (i = 0; i < 24; i++) {
                double m = (a + b) / 2;
                if (vowel_f1_at(e, front, m, round) < want) a = m; else b = m;
            }
            o = (a + b) / 2;
        }
    }
    vowel_formants(e, front, o, round, F, rhotic);
    for (n = 0; n < 5; n++) F[n] *= s;
    if (tuned) F[0] = f0;
    ceil = 0.45 * e->rate;
    for (n = 1; n < 5; n++) if (F[n] < F[n - 1] * 1.15) F[n] = F[n - 1] * 1.15;
    for (n = 0; n < 5; n++) {
        if (F[n] > ceil) F[n] = ceil;
        B[n] = dmax(F[n] / 10, 50);
    }
    return s;
}

/* ── the consonants ── */

#define MANNER_STOP 1
#define MANNER_AFFRICATE 2
#define MANNER_FRICATIVE 3
#define MANNER_ASPIRATE 4
#define MANNER_NASAL 5
#define MANNER_APPROX 6
#define MANNER_TAP 7
#define MANNER_TRILL 8
#define MANNER_GLOTTAL 9

typedef struct TaudConsonant {
    int id, manner;
    bool voiced;
    double F[3], B[3], bank[6];
    double lvl, dur, burst, vot, trans, av, fnz;
} TaudConsonant;

/* Generated from formant.js's ROWS: id, manner, voiced, F, B, bank (A2…A6,
 * AB), lvl, dur, burst, vot, trans, av, fnz. */
static const TaudConsonant ROWS[] = {
    { 1, 1, 0, { 400, 1100, 2150 }, { 300, 150, 220 }, { 0, 0, 0, 0, 0, 63 }, -12, 0, 5, 12, 40, 0, 0 }, /* p */
    { 2, 1, 1, { 200, 1100, 2150 }, { 60, 110, 130 }, { 0, 0, 0, 0, 0, 63 }, -14, 0, 5, 0, 40, 0, 0 }, /* b */
    { 3, 1, 0, { 400, 1600, 2600 }, { 300, 120, 250 }, { 0, 30, 45, 57, 63, 0 }, -10, 0, 10, 18, 45, 0, 0 }, /* t */
    { 4, 1, 1, { 200, 1600, 2600 }, { 60, 100, 170 }, { 0, 47, 60, 62, 60, 0 }, -12, 0, 8, 0, 45, 0, 0 }, /* d */
    { 5, 1, 0, { 400, 1500, 1900 }, { 300, 120, 250 }, { 0, 50, 55, 50, 45, 0 }, -10, 0, 10, 18, 45, 0, 0 }, /* ʈ */
    { 6, 1, 1, { 200, 1500, 1900 }, { 60, 100, 170 }, { 0, 50, 55, 50, 45, 0 }, -12, 0, 8, 0, 45, 0, 0 }, /* ɖ */
    { 7, 1, 0, { 300, 2100, 2900 }, { 250, 160, 330 }, { 0, 45, 55, 50, 40, 0 }, -10, 0, 15, 22, 50, 0, 0 }, /* c */
    { 8, 1, 1, { 200, 2100, 2900 }, { 60, 150, 280 }, { 0, 45, 55, 50, 40, 0 }, -12, 0, 12, 0, 50, 0, 0 }, /* ɟ */
    { 9, 1, 0, { 300, 1990, 2850 }, { 250, 160, 330 }, { 0, 53, 43, 45, 45, 0 }, -10, 0, 20, 25, 50, 0, 0 }, /* k */
    { 10, 1, 1, { 200, 1990, 2850 }, { 60, 150, 280 }, { 0, 53, 43, 45, 45, 0 }, -12, 0, 15, 0, 50, 0, 0 }, /* g */
    { 11, 1, 0, { 400, 1200, 2400 }, { 250, 150, 300 }, { 55, 45, 35, 30, 30, 0 }, -10, 0, 20, 25, 50, 0, 0 }, /* q */
    { 12, 1, 1, { 250, 1200, 2400 }, { 60, 120, 250 }, { 55, 45, 35, 30, 30, 0 }, -12, 0, 15, 0, 50, 0, 0 }, /* ɢ */
    { 13, 9, 0, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0 }, /* ʔ */
    { 14, 1, 0, { 400, 1100, 2150 }, { 300, 150, 220 }, { 0, 0, 0, 0, 0, 63 }, -12, 0, 5, 58, 40, 0, 0 }, /* pʰ */
    { 15, 1, 0, { 400, 1600, 2600 }, { 300, 120, 250 }, { 0, 30, 45, 57, 63, 0 }, -10, 0, 10, 70, 45, 0, 0 }, /* tʰ */
    { 16, 1, 0, { 300, 1990, 2850 }, { 250, 160, 330 }, { 0, 53, 43, 45, 45, 0 }, -10, 0, 20, 80, 50, 0, 0 }, /* kʰ */
    { 32, 5, 1, { 480, 1270, 2130 }, { 40, 200, 200 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 35, 0.5, 450 }, /* m */
    { 33, 5, 1, { 480, 1200, 2100 }, { 40, 200, 200 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 35, 0.5, 450 }, /* ɱ */
    { 34, 5, 1, { 480, 1340, 2470 }, { 40, 300, 300 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 35, 0.5, 450 }, /* n */
    { 35, 5, 1, { 480, 1400, 1900 }, { 40, 300, 300 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 35, 0.5, 450 }, /* ɳ */
    { 36, 5, 1, { 480, 2000, 2900 }, { 40, 300, 300 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 40, 0.5, 450 }, /* ɲ */
    { 37, 5, 1, { 480, 1900, 2700 }, { 40, 300, 300 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 40, 0.5, 450 }, /* ŋ */
    { 38, 5, 1, { 480, 1200, 2400 }, { 40, 300, 300 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 40, 0.5, 450 }, /* ɴ */
    { 40, 8, 1, { 300, 1100, 2150 }, { 70, 110, 130 }, { 0, 0, 0, 0, 0, 0 }, 0, 40, 0, 0, 50, 0, 0 }, /* ʙ */
    { 41, 8, 1, { 300, 1500, 2500 }, { 70, 100, 150 }, { 0, 0, 0, 0, 0, 0 }, 0, 40, 0, 0, 50, 0, 0 }, /* r */
    { 42, 8, 1, { 400, 1200, 2300 }, { 80, 120, 200 }, { 0, 0, 0, 0, 0, 0 }, 0, 40, 0, 0, 50, 0, 0 }, /* ʀ */
    { 44, 7, 1, { 300, 1100, 2080 }, { 60, 90, 120 }, { 0, 0, 0, 0, 0, 0 }, 0, 20, 0, 0, 35, 0, 0 }, /* ⱱ */
    { 45, 7, 1, { 300, 1600, 2600 }, { 60, 100, 170 }, { 0, 0, 0, 0, 0, 0 }, 0, 20, 0, 0, 35, 0, 0 }, /* ɾ */
    { 46, 7, 1, { 300, 1500, 1900 }, { 60, 100, 170 }, { 0, 0, 0, 0, 0, 0 }, 0, 20, 0, 0, 35, 0, 0 }, /* ɽ */
    { 48, 3, 0, { 340, 1000, 2100 }, { 200, 120, 150 }, { 0, 0, 0, 0, 0, 55 }, -24, 100, 0, 0, 40, 0, 0 }, /* ɸ */
    { 49, 3, 1, { 220, 1000, 2100 }, { 60, 90, 120 }, { 0, 0, 0, 0, 0, 55 }, -28, 70, 0, 0, 40, 0.35, 0 }, /* β */
    { 50, 3, 0, { 340, 1100, 2080 }, { 200, 120, 150 }, { 0, 0, 0, 0, 0, 57 }, -22, 100, 0, 0, 40, 0, 0 }, /* f */
    { 51, 3, 1, { 220, 1100, 2080 }, { 60, 90, 120 }, { 0, 0, 0, 0, 0, 57 }, -26, 70, 0, 0, 40, 0.35, 0 }, /* v */
    { 52, 3, 0, { 320, 1290, 2540 }, { 200, 90, 200 }, { 0, 0, 0, 0, 28, 48 }, -24, 100, 0, 0, 40, 0, 0 }, /* θ */
    { 53, 3, 1, { 270, 1290, 2540 }, { 60, 80, 170 }, { 0, 0, 0, 0, 28, 48 }, -28, 70, 0, 0, 40, 0.35, 0 }, /* ð */
    { 54, 3, 0, { 320, 1390, 2530 }, { 200, 80, 200 }, { 0, 0, 0, 0, 52, 0 }, -8, 120, 0, 0, 40, 0, 0 }, /* s */
    { 55, 3, 1, { 240, 1390, 2530 }, { 70, 60, 180 }, { 0, 0, 0, 0, 52, 0 }, -12, 90, 0, 0, 40, 0.35, 0 }, /* z */
    { 56, 3, 0, { 300, 1840, 2750 }, { 200, 100, 300 }, { 0, 57, 48, 48, 46, 0 }, -6, 120, 0, 0, 40, 0, 0 }, /* ʃ */
    { 57, 3, 1, { 240, 1840, 2750 }, { 60, 100, 300 }, { 0, 57, 48, 48, 46, 0 }, -10, 90, 0, 0, 40, 0.35, 0 }, /* ʒ */
    { 58, 3, 0, { 300, 1600, 2000 }, { 200, 100, 250 }, { 0, 57, 50, 40, 30, 0 }, -7, 120, 0, 0, 40, 0, 0 }, /* ʂ */
    { 59, 3, 1, { 240, 1600, 2000 }, { 60, 100, 250 }, { 0, 57, 50, 40, 30, 0 }, -11, 90, 0, 0, 40, 0.35, 0 }, /* ʐ */
    { 60, 3, 0, { 300, 2100, 3000 }, { 200, 150, 300 }, { 0, 35, 55, 50, 40, 0 }, -12, 110, 0, 0, 45, 0, 0 }, /* ç */
    { 61, 3, 1, { 240, 2100, 3000 }, { 60, 150, 300 }, { 0, 35, 55, 50, 40, 0 }, -16, 80, 0, 0, 45, 0.35, 0 }, /* ʝ */
    { 62, 3, 0, { 300, 1700, 2600 }, { 200, 150, 300 }, { 57, 50, 40, 35, 30, 0 }, -14, 110, 0, 0, 45, 0, 0 }, /* x */
    { 63, 3, 1, { 240, 1700, 2600 }, { 60, 150, 300 }, { 57, 50, 40, 35, 30, 0 }, -18, 80, 0, 0, 45, 0.35, 0 }, /* ɣ */
    { 64, 3, 0, { 400, 1200, 2400 }, { 200, 150, 300 }, { 58, 45, 35, 30, 25, 0 }, -13, 110, 0, 0, 45, 0, 0 }, /* χ */
    { 65, 3, 1, { 300, 1200, 2400 }, { 80, 150, 300 }, { 58, 45, 35, 30, 25, 0 }, -17, 80, 0, 0, 45, 0.35, 0 }, /* ʁ */
    { 66, 4, 0, { 700, 1100, 2500 }, { 300, 150, 300 }, { 0, 0, 0, 0, 0, 0 }, -16, 90, 0, 0, 30, 0, 0 }, /* ħ */
    { 67, 4, 1, { 700, 1100, 2500 }, { 100, 150, 300 }, { 0, 0, 0, 0, 0, 0 }, -20, 70, 0, 0, 30, 0.4, 0 }, /* ʕ */
    { 68, 4, 0, { 0, 0, 0 }, { 300, 0, 0 }, { 0, 0, 0, 0, 0, 0 }, -16, 70, 0, 0, 15, 0, 0 }, /* h */
    { 69, 4, 1, { 0, 0, 0 }, { 300, 0, 0 }, { 0, 0, 0, 0, 0, 0 }, -20, 60, 0, 0, 15, 0.4, 0 }, /* ɦ */
    { 70, 3, 0, { 320, 1300, 2600 }, { 200, 90, 200 }, { 0, 0, 45, 50, 48, 40 }, -12, 110, 0, 0, 45, 0, 0 }, /* ɬ */
    { 71, 3, 1, { 240, 1300, 2600 }, { 60, 90, 200 }, { 0, 0, 45, 50, 48, 40 }, -16, 80, 0, 0, 45, 0.35, 0 }, /* ɮ */
    { 72, 3, 0, { 300, 2000, 2900 }, { 200, 120, 300 }, { 0, 45, 57, 50, 45, 0 }, -7, 120, 0, 0, 45, 0, 0 }, /* ɕ */
    { 73, 3, 1, { 240, 2000, 2900 }, { 60, 120, 300 }, { 0, 45, 57, 50, 45, 0 }, -11, 90, 0, 0, 45, 0.35, 0 }, /* ʑ */
    { 74, 3, 0, { 290, 610, 2150 }, { 200, 80, 60 }, { 0, 0, 0, 0, 0, 50 }, -22, 90, 0, 0, 100, 0, 0 }, /* ʍ */
    { 80, 6, 1, { 300, 1100, 2200 }, { 60, 90, 150 }, { 0, 0, 0, 0, 0, 0 }, 0, 50, 0, 0, 70, 0.4, 0 }, /* ʋ */
    { 81, 6, 1, { 310, 1060, 1380 }, { 70, 100, 120 }, { 0, 0, 0, 0, 0, 0 }, 0, 50, 0, 0, 90, 0.4, 0 }, /* ɹ */
    { 82, 6, 1, { 310, 1100, 1500 }, { 70, 100, 120 }, { 0, 0, 0, 0, 0, 0 }, 0, 50, 0, 0, 90, 0.4, 0 }, /* ɻ */
    { 83, 6, 1, { 260, 2070, 3020 }, { 40, 250, 500 }, { 0, 0, 0, 0, 0, 0 }, 0, 40, 0, 0, 100, 0.4, 0 }, /* j */
    { 84, 6, 1, { 300, 1200, 2400 }, { 50, 90, 150 }, { 0, 0, 0, 0, 0, 0 }, 0, 40, 0, 0, 100, 0.4, 0 }, /* ɰ */
    { 85, 6, 1, { 290, 610, 2150 }, { 50, 80, 60 }, { 0, 0, 0, 0, 0, 0 }, 0, 40, 0, 0, 100, 0.4, 0 }, /* w */
    { 86, 6, 1, { 260, 1700, 2200 }, { 40, 200, 300 }, { 0, 0, 0, 0, 0, 0 }, 0, 40, 0, 0, 100, 0.4, 0 }, /* ɥ */
    { 88, 6, 1, { 310, 1050, 2880 }, { 50, 100, 280 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 60, 0.4, 0 }, /* l */
    { 89, 6, 1, { 310, 1300, 1900 }, { 50, 100, 200 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 60, 0.4, 0 }, /* ɭ */
    { 90, 6, 1, { 300, 2000, 2800 }, { 50, 150, 250 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 70, 0.4, 0 }, /* ʎ */
    { 91, 6, 1, { 310, 1100, 2700 }, { 50, 100, 250 }, { 0, 0, 0, 0, 0, 0 }, 0, 60, 0, 0, 70, 0.4, 0 }, /* ʟ */
    { 96, 2, 0, { 320, 1500, 2550 }, { 200, 90, 200 }, { 0, 0, 0, 0, 52, 0 }, -8, 70, 8, 0, 40, 0, 0 }, /* t͡s */
    { 97, 2, 1, { 240, 1500, 2550 }, { 70, 60, 180 }, { 0, 0, 0, 0, 52, 0 }, -12, 60, 8, 0, 40, 0, 0 }, /* d͡z */
    { 98, 2, 0, { 350, 1800, 2820 }, { 200, 90, 300 }, { 0, 44, 60, 53, 53, 0 }, -6, 70, 8, 0, 40, 0, 0 }, /* t͡ʃ */
    { 99, 2, 1, { 260, 1800, 2820 }, { 60, 80, 270 }, { 0, 44, 60, 53, 53, 0 }, -10, 60, 8, 0, 40, 0, 0 }, /* d͡ʒ */
    { 100, 2, 0, { 300, 2000, 2900 }, { 200, 120, 300 }, { 0, 45, 57, 50, 45, 0 }, -7, 70, 10, 0, 45, 0, 0 }, /* t͡ɕ */
    { 101, 2, 1, { 240, 2000, 2900 }, { 60, 120, 300 }, { 0, 45, 57, 50, 45, 0 }, -11, 60, 10, 0, 45, 0, 0 }, /* d͡ʑ */
    { 102, 2, 0, { 300, 1600, 2000 }, { 200, 100, 250 }, { 0, 57, 50, 40, 30, 0 }, -7, 70, 10, 0, 45, 0, 0 }, /* ʈ͡ʂ */
    { 103, 2, 1, { 240, 1600, 2000 }, { 60, 100, 250 }, { 0, 57, 50, 40, 30, 0 }, -11, 60, 10, 0, 45, 0, 0 }, /* ɖ͡ʐ */
    { 104, 2, 0, { 340, 1100, 2080 }, { 200, 120, 150 }, { 0, 0, 0, 0, 0, 57 }, -16, 70, 5, 0, 40, 0, 0 }, /* p͡f */
    { 105, 2, 0, { 300, 1800, 2700 }, { 250, 160, 330 }, { 57, 50, 40, 35, 30, 0 }, -12, 70, 20, 0, 50, 0, 0 }, /* k͡x */
};
#define ROW_COUNT ((int)(sizeof ROWS / sizeof ROWS[0]))

static const TaudConsonant *consonant(int id)
{
    int i;
    for (i = 0; i < ROW_COUNT; i++) if (ROWS[i].id == id) return &ROWS[i];
    return NULL;
}

/* The glides: [code] → the syllabic vowel's chart point and the row. */
static const struct { int front, open, round, row; } SEMIVOWELS[5] = {
    { 0, 0, 0, 0 },
    { 255, 0, 0, 83 },
    { 255, 0, 255, 86 },
    { 0, 0, 0, 84 },
    { 0, 0, 255, 85 },
};

static bool consonant_needs_noise(const TaudConsonant *r)
{
    return r != NULL && (r->manner == MANNER_STOP || r->manner == MANNER_AFFRICATE ||
                         r->manner == MANNER_FRICATIVE || r->manner == MANNER_ASPIRATE);
}

#define CLOSURE_MS 50
#define ASPIRATION_DB (-6)
static double db_gain(double db) { return js_pow(10, db / 20); }

/* ── segment tracks ── */

static void tr_reset(TaudFormantTrack *t) { t->n = 0; }

static TaudFormantTrack *tr_at(TaudFormantTrack *t, double time, double v)
{
    if (t->n >= 2 && time < t->p[t->n - 2]) time = t->p[t->n - 2];
    if (t->n + 2 <= FORMANT_TRACK_MAX) { t->p[t->n] = time; t->p[t->n + 1] = v; t->n += 2; }
    return t;
}

static void tr_close(TaudFormantTrack *t, double time, double v)
{
    double lt;
    if (t->n == 0) tr_at(t, 0, v);
    lt = t->p[t->n - 2];
    if (time > lt) tr_at(t, time, v);
    else if (t->p[t->n - 1] != v) tr_at(t, lt, v);
}

static double track_at(const TaudFormantTrack *tr, double t)
{
    const double *p = tr->p;
    int i;
    if (t <= p[0]) return p[1];
    for (i = 2; i < tr->n; i += 2) {
        if (t < p[i]) {
            double t0 = p[i - 2], t1 = p[i], v0 = p[i - 1], v1 = p[i + 1];
            return t1 > t0 ? v0 + ((v1 - v0) * (t - t0)) / (t1 - t0) : v1;
        }
    }
    return p[tr->n - 1];
}

typedef struct { double av, ah, af, nz; } NextStart;

/* An onset consonant's tracks (onsetTracks). */
static void onset_tracks(const TaudConsonant *r, bool first, const NextStart *nx, double k, TaudFormantSegment *sg)
{
    TaudFormantTrack *w = &sg->w, *av = &sg->av, *ah = &sg->ah, *af = &sg->af, *nz = &sg->nz;
    double L = db_gain(r->lvl);
    double tr = r->trans * k, dur = r->dur * k, end;
    tr_reset(w); tr_reset(av); tr_reset(ah); tr_reset(af); tr_reset(nz);
    switch (r->manner) {
    case MANNER_STOP: {
        double cl = first ? 0 : CLOSURE_MS * k;
        double tb = cl + r->burst * k;
        double tv = tb + r->vot * k;
        if (r->voiced) {
            tr_at(tr_at(tr_at(tr_at(av, 0, cl > 0 ? 0.25 : 0.3), cl, cl > 0 ? 0.25 : 0.3), tb, 0.3), tb + 10 * k, nx->av);
        } else {
            tr_at(tr_at(tr_at(av, 0, 0), tv, 0), tv + 10 * k, nx->av);
        }
        tr_at(tr_at(tr_at(tr_at(tr_at(af, 0, 0), cl, 0), cl, L), tb, 0.3 * L), tb + 2 * k, 0);
        tr_at(ah, 0, 0);
        if (!r->voiced && r->vot > 0) {
            double La = L * db_gain(ASPIRATION_DB);
            tr_at(tr_at(tr_at(tr_at(ah, tb, 0), tb + 3 * k, La), tv, 0.7 * La), tv + 10 * k, nx->ah);
        }
        tr_at(tr_at(tr_at(tr_at(w, 0, 1), tb, 1), tb + 1 * k, 0.5), tb + tr, 0);
        tr_at(nz, 0, 0);
        end = dmax(tv + 10 * k, tb + tr);
        break;
    }
    case MANNER_AFFRICATE: {
        double cl = first ? 0 : CLOSURE_MS * k;
        double tb = cl + r->burst * k;
        double tf = tb + dur;
        tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(af, 0, 0), cl, 0), cl, L), tb, 0.8 * L), tf - 10 * k, L), tf + 10 * k, nx->af);
        if (r->voiced) tr_at(tr_at(tr_at(av, 0, 0.3), tf, 0.3), tf + 15 * k, nx->av);
        else tr_at(tr_at(tr_at(av, 0, 0), tf, 0), tf + 15 * k, nx->av);
        tr_at(ah, 0, 0);
        tr_at(tr_at(tr_at(w, 0, 1), tf, 1), tf + tr, 0);
        tr_at(nz, 0, 0);
        end = dmax(tf + 15 * k, tf + tr);
        break;
    }
    case MANNER_FRICATIVE:
        tr_at(tr_at(tr_at(tr_at(af, 0, 0), 10 * k, L), dur - 10 * k, L), dur + 10 * k, nx->af);
        if (r->voiced) tr_at(tr_at(tr_at(av, 0, r->av), dur, r->av), dur + 15 * k, nx->av);
        else tr_at(tr_at(tr_at(av, 0, 0), dur - 5 * k, 0), dur + 15 * k, nx->av);
        tr_at(ah, 0, 0);
        tr_at(tr_at(tr_at(w, 0, 1), dur, 1), dur + tr, 0);
        tr_at(nz, 0, 0);
        end = dmax(dur + 15 * k, dur + tr);
        break;
    case MANNER_ASPIRATE:
        tr_at(tr_at(tr_at(tr_at(ah, 0, 0), 10 * k, L), dur - 10 * k, L), dur + 10 * k, nx->ah);
        if (r->voiced) tr_at(tr_at(tr_at(av, 0, r->av), dur, r->av), dur + 10 * k, nx->av);
        else tr_at(tr_at(tr_at(av, 0, 0), dur - 10 * k, 0), dur + 10 * k, nx->av);
        tr_at(af, 0, 0);
        tr_at(tr_at(tr_at(w, 0, 1), dur, 1), dur + tr, 0);
        tr_at(nz, 0, 0);
        end = dmax(dur + 10 * k, dur + tr);
        break;
    case MANNER_NASAL:
        tr_at(tr_at(tr_at(av, 0, r->av), dur, r->av), dur + 10 * k, nx->av);
        tr_at(tr_at(tr_at(nz, 0, 1), dur, 1), dur + 10 * k, nx->nz);
        tr_at(tr_at(tr_at(tr_at(w, 0, 1), dur, 1), dur + 5 * k, 0.5), dur + tr, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0);
        end = dmax(dur + 10 * k, dur + tr);
        break;
    case MANNER_APPROX:
        tr_at(tr_at(tr_at(av, 0, r->av), dur, r->av), dur + tr * 0.5, nx->av);
        tr_at(tr_at(tr_at(w, 0, 1), dur, 1), dur + tr, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = dur + tr;
        break;
    case MANNER_TAP:
        tr_at(tr_at(tr_at(av, 0, 0.2), dur, 0.2), dur + 5 * k, nx->av);
        tr_at(tr_at(tr_at(w, 0, 1), dur, 1), dur + tr, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = dmax(dur + 5 * k, dur + tr);
        break;
    case MANNER_TRILL: {
        double c = dur, shut = c * 0.375, tail;
        tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(av, 0, 0.12), shut, 0.12), shut + 3 * k, 0.7), c - 3 * k, 0.7),
              c, 0.12), c + shut, 0.12), c + shut + 3 * k, nx->av);
        tail = c + shut + 3 * k;
        tr_at(tr_at(tr_at(w, 0, 1), tail, 1), tail + tr, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = tail + tr;
        break;
    }
    default: {
        double cl = first ? 0 : 30 * k;
        tr_at(tr_at(tr_at(av, 0, 0), cl, 0), cl + 3 * k, nx->av);
        tr_at(w, 0, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = cl + 3 * k;
        break;
    }
    }
    sg->end = end;
    tr_close(w, end, 0);
    tr_close(av, end, nx->av);
    tr_close(ah, end, nx->ah);
    tr_close(af, end, nx->af);
    tr_close(nz, end, nx->nz);
}

/* A coda consonant's tracks (codaTracks). */
static void coda_tracks(const TaudConsonant *r, double k, TaudFormantSegment *sg)
{
    TaudFormantTrack *w = &sg->w, *av = &sg->av, *ah = &sg->ah, *af = &sg->af, *nz = &sg->nz;
    double L = db_gain(r->lvl);
    double tr = r->trans * k, dur = r->dur * k, end;
    tr_reset(w); tr_reset(av); tr_reset(ah); tr_reset(af); tr_reset(nz);
    switch (r->manner) {
    case MANNER_STOP:
    case MANNER_AFFRICATE: {
        double tc = tr + CLOSURE_MS * k;
        double tb = tc + r->burst * k;
        tr_at(tr_at(tr_at(w, 0, 0), tr, 0.5), tr + 5 * k, 1);
        if (r->voiced) tr_at(tr_at(tr_at(tr_at(tr_at(av, 0, 1), tr, 1), tr + 8 * k, 0.25), tc, 0.2), tc + 5 * k, 0);
        else tr_at(tr_at(tr_at(av, 0, 1), tr, 1), tr + 8 * k, 0);
        if (r->manner == MANNER_STOP) {
            tr_at(tr_at(tr_at(tr_at(tr_at(af, 0, 0), tc, 0), tc, 0.7 * L), tb, 0.2 * L), tb + 2 * k, 0);
            tr_at(ah, 0, 0);
            if (!r->voiced && r->vot > 0) {
                double La = L * db_gain(ASPIRATION_DB);
                tr_at(tr_at(tr_at(ah, tb, 0), tb + 2 * k, 0.5 * La), tb + 30 * k, 0);
            }
            end = tb + 35 * k;
        } else {
            tr_at(tr_at(tr_at(tr_at(tr_at(af, 0, 0), tc, 0), tc, L), tb + dur, 0.6 * L), tb + dur + 20 * k, 0);
            tr_at(ah, 0, 0);
            end = tb + dur + 20 * k;
        }
        tr_at(nz, 0, 0);
        break;
    }
    case MANNER_FRICATIVE: {
        double a = dmax(tr - 15 * k, 0);
        tr_at(tr_at(w, 0, 0), tr, 1);
        if (r->voiced) tr_at(tr_at(tr_at(tr_at(av, 0, 1), tr, r->av), tr + dur, 0.7 * r->av), tr + dur + 30 * k, 0);
        else tr_at(tr_at(tr_at(av, 0, 1), a, 1), tr + 5 * k, 0);
        tr_at(tr_at(tr_at(tr_at(tr_at(af, 0, 0), a, 0), tr + 5 * k, L), tr + dur, 0.8 * L), tr + dur + 30 * k, 0);
        tr_at(ah, 0, 0); tr_at(nz, 0, 0);
        end = tr + dur + 30 * k;
        break;
    }
    case MANNER_ASPIRATE:
        tr_at(tr_at(w, 0, 0), 15 * k, 1);
        if (r->voiced) tr_at(tr_at(tr_at(tr_at(av, 0, 1), 20 * k, r->av), dur, 0.5 * r->av), dur + 20 * k, 0);
        else tr_at(tr_at(av, 0, 1), 20 * k, 0);
        tr_at(tr_at(tr_at(tr_at(ah, 0, 0), 10 * k, L), dur, 0.5 * L), dur + 30 * k, 0);
        tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = dur + 30 * k;
        break;
    case MANNER_NASAL:
        tr_at(tr_at(w, 0, 0), tr, 1);
        tr_at(tr_at(tr_at(nz, 0, 0), dmax(tr - 5 * k, 0), 0), tr, 1);
        tr_at(tr_at(tr_at(tr_at(av, 0, 1), tr, r->av), tr + dur, 0.8 * r->av), tr + dur + 40 * k, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0);
        end = tr + dur + 40 * k;
        break;
    case MANNER_APPROX:
        tr_at(tr_at(w, 0, 0), tr, 1);
        tr_at(tr_at(tr_at(tr_at(av, 0, 1), tr, r->av), tr + dur, 0.8 * r->av), tr + dur + 40 * k, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = tr + dur + 40 * k;
        break;
    case MANNER_TAP:
        tr_at(tr_at(w, 0, 0), tr, 1);
        tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(av, 0, 1), tr, 1), tr + 3 * k, 0.2), tr + dur, 0.2), tr + dur + 3 * k, 0.6),
              tr + dur + 40 * k, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = tr + dur + 40 * k;
        break;
    case MANNER_TRILL: {
        double c = dur, shut = c * 0.375;
        tr_at(tr_at(w, 0, 0), tr, 1);
        tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(tr_at(av, 0, 1), tr, 0.7), tr + 3 * k, 0.12),
              tr + shut, 0.12), tr + shut + 3 * k, 0.7), tr + c - 3 * k, 0.7), tr + c, 0.12), tr + c + shut, 0.12),
              tr + c + shut + 3 * k, 0.6), tr + c + shut + 35 * k, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = tr + c + shut + 35 * k;
        break;
    }
    default:
        tr_at(w, 0, 0);
        tr_at(tr_at(av, 0, 1), 6 * k, 0);
        tr_at(ah, 0, 0); tr_at(af, 0, 0); tr_at(nz, 0, 0);
        end = 6 * k;
        break;
    }
    sg->end = end;
    tr_close(w, end, 1);
    tr_close(av, end, 0);
    tr_close(ah, end, 0);
    tr_close(af, end, 0);
    tr_close(nz, end, 0);
}

static double timing_scale(int b) { return js_pow(2, (b - 128) / 64.0); }

/* ── loudness tables ── */

#define NORM_EVERY 8
#define NORM_MIN (1.0 / 256)
#define NORM_MAX 16
#define FORMANT_LEVEL 0.5773502691896258
#define SOURCE_GAIN_MAX 16
#define NOISE_SEED 0x9e3779b9u
#define SOURCE_MEASURE_MAX 16384
#define SOURCE_WINDOW 1024
#define FNP 270
#define BNP 100
#define BNZ 100
#define LOUDNESS_PHON 60

static const double ISO226_F[ISO226_N] = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400,
    500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500 };
static const double ISO226_AF[ISO226_N] = { 0.532, 0.506, 0.480, 0.455, 0.432, 0.409, 0.387, 0.367, 0.349,
    0.330, 0.315, 0.301, 0.288, 0.276, 0.267, 0.259, 0.253, 0.250, 0.246, 0.244, 0.243, 0.243, 0.243, 0.242,
    0.242, 0.245, 0.254, 0.271, 0.301 };
static const double ISO226_LU[ISO226_N] = { -31.6, -27.2, -23.0, -19.1, -15.9, -13.0, -10.3, -8.1, -6.2, -4.5,
    -3.1, -2.0, -1.1, -0.4, 0.0, 0.3, 0.5, 0.0, -2.7, -4.1, -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1 };
static const double ISO226_TF[ISO226_N] = { 78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4,
    11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7, -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3 };
#define ELW_LAST (ISO226_N - 1)

/* The equal-loudness weighting in dB at f, searching up from index *j. */
static double elw_db(const TaudFormantTables *T, double f, int *j)
{
    double lf = js_log2(f), u;
    int i = *j;
    if (!(lf > T->elwLogF[0])) { *j = 0; return T->elwDb[0]; }
    if (lf >= T->elwLogF[ELW_LAST]) { *j = ELW_LAST; return T->elwDb[ELW_LAST]; }
    while (lf > T->elwLogF[i + 1]) i++;
    *j = i;
    u = (lf - T->elwLogF[i]) / (T->elwLogF[i + 1] - T->elwLogF[i]);
    return T->elwDb[i] + u * (T->elwDb[i + 1] - T->elwDb[i]);
}

void te_formant_tables_init(TaudFormantTables *T, int rate)
{
    double band;
    int i, h, c, n;
    for (h = 0; h < 4; h++)
        for (c = 0; c < 4; c++)
            for (n = 0; n < 2; n++) T->logCard[h][c][n] = js_log(CARDINAL_VOWELS[h][c][n]);
    for (i = 0; i < ISO226_N; i++) {
        double af = ISO226_AF[i], lu = ISO226_LU[i], tf = ISO226_TF[i];
        double A = 4.47e-3 * (js_pow(10, 0.025 * LOUDNESS_PHON) - 1.15) +
                   js_pow(0.4 * js_pow(10, (tf + lu) / 10 - 9), af);
        T->elwLogF[i] = js_log2(ISO226_F[i]);
        T->elwDb[i] = -((10 / af) * js_log10(A) - lu + 94);
    }
    band = js_pow(2, 1.0 / 12) - js_pow(2, -1.0 / 12);
    T->bankN = 0;
    T->bankRef = 0;
    for (i = 0; 50 * js_pow(2, i / 6.0) <= 0.45 * rate && i < BANK_GRID_MAX; i++) {
        double f = 50 * js_pow(2, i / 6.0), om;
        int j = 0;
        T->bankW[i] = js_pow(10, elw_db(T, f, &j) / 10) * f * band;
        om = (2 * PI * f) / rate;
        T->bankC1[i] = js_cos(om);
        T->bankS1[i] = js_sin(om);
        T->bankC2[i] = js_cos(2 * om);
        T->bankS2[i] = js_sin(2 * om);
        T->bankN = i + 1;
    }
    for (i = 0; i < T->bankN; i++) T->bankRef = T->bankRef + T->bankW[i];
}

static const double PAR_F[3] = { 3300, 3750, 4900 }, PAR_B[3] = { 250, 200, 1000 };

/* ── the sounding voice ── */

static void start_noise(const TaudEngine *e, TaudFormantState *st, const TaudFormantSegment *seg);

static void next_start_of(const TaudFormantSegment *s, NextStart *nx)
{
    nx->av = s->av.p[1]; nx->ah = s->ah.p[1]; nx->af = s->af.p[1]; nx->nz = s->nz.p[1];
}

/* A fresh tract, as `new FormantState()` leaves one. */
void te_formant_state_init(TaudFormantState *st)
{
    memset(st, 0, sizeof *st);
    st->phase = PHASE_DONE;
    st->fresh = true;
    st->upd = 48;
    st->nv = 1.0; st->nvGoal = 1.0;
    st->sg = 1.0;
    st->sgDue = true;
    st->nGain = 1.0;
    st->nSeed = NOISE_SEED;
}

void te_formant_note_on(TaudEngine *e, TaudFormantState *st, const TaudFormantPatch *patch)
{
    double k = timing_scale(patch->timing);
    const TaudConsonant *rows[2];
    int rowSlot[2], nrows = 0, i, next;
    NextStart nx;
    const TaudConsonant *cr;
    st->patch = patch;
    for (i = 0; i < 2; i++) {
        const TaudConsonant *r = consonant(patch->slots[i].cons);
        if (r != NULL) { rows[nrows] = r; rowSlot[nrows] = i; nrows++; }
    }
    st->sv = patch->semivowel;
    st->svT = 0.0;
    if (st->sv != 0) {
        const TaudConsonant *r = consonant(SEMIVOWELS[st->sv].row);
        double dur = r->dur * k, tr = r->trans * k;
        st->svEnd = dur + tr;
        tr_reset(&st->svW);
        tr_close(tr_at(tr_at(tr_at(&st->svW, 0, 1), dur, 1), dur + tr, 0), st->svEnd, 0);
        tr_reset(&st->svAv);
        tr_close(tr_at(tr_at(tr_at(&st->svAv, 0, r->av), dur, r->av), dur + tr * 0.5, 1), st->svEnd, 1);
        nx.av = st->svAv.p[1]; nx.ah = 0; nx.af = 0; nx.nz = 0;
    } else {
        nx.av = 1; nx.ah = 0; nx.af = 0; nx.nz = 0;
    }
    next = -1;
    for (i = nrows - 1; i >= 0; i--) {
        TaudFormantSegment *s = &st->onset[i];
        s->row = rows[i];
        s->slot = patch->slots[rowSlot[i]];
        onset_tracks(rows[i], i == 0, &nx, k, s);
        s->next = next;
        next = i;
        next_start_of(s, &nx);
    }
    st->onsetCount = nrows;
    st->seg = 0;
    cr = consonant(patch->slots[2].cons);
    st->hasCoda = cr != NULL;
    if (cr != NULL) {
        st->coda.row = cr;
        st->coda.slot = patch->slots[2];
        st->coda.next = -1;
        coda_tracks(cr, k, &st->coda);
    }
    st->phase = nrows > 0 ? PHASE_ONSET : PHASE_VOWEL;
    st->t = 0.0;
    st->codaVeto = false;
    st->fresh = true;
    st->upd = imax(1, (int)js_round(e->rate / 1000.0));
    st->ctl = 0;
    st->normCount = 0;
    st->ownVowel = false; st->rejoin = false;
    st->vFront = patch->front; st->vOpen = patch->open; st->vRound = patch->round;
    st->glideLeft = 0.0;
    st->sgDue = true;
    st->av = 0.0; st->avStep = 0.0; st->ah = 0.0; st->ahStep = 0.0; st->af = 0.0; st->afStep = 0.0;
    st->nv = 1.0; st->nvStep = 0.0; st->nvGoal = 1.0;
    memset(st->cy, 0, sizeof st->cy);
    memset(st->by, 0, sizeof st->by);
    st->nOn = false; st->nZ = 0.0;
    if (st->phase == PHASE_ONSET) start_noise(e, st, &st->onset[0]);
}

void te_formant_restart(TaudEngine *e, TaudFormantState *st)
{
    double f, o, r, sg, sm;
    bool own, due;
    if (st->patch == NULL) return;
    f = st->vFront; o = st->vOpen; r = st->vRound; own = st->ownVowel;
    sg = st->sg; sm = st->sm; due = st->sgDue;
    te_formant_note_on(e, st, st->patch);
    st->vFront = f; st->vOpen = o; st->vRound = r; st->ownVowel = own;
    st->sg = sg; st->sm = sm; st->sgDue = due;
}

static void formant_glide_to(TaudFormantState *st, double open, double front, double round)
{
    double ms = (st->patch != NULL ? st->patch->glide : 0) * 2;
    if (st->fresh || ms <= 0) {
        st->vOpen = open; st->vFront = front; st->vRound = round;
        st->glideLeft = 0.0;
        return;
    }
    st->gOpen = (open - st->vOpen) / ms;
    st->gFront = (front - st->vFront) / ms;
    st->gRound = (round - st->vRound) / ms;
    st->tOpen = open; st->tFront = front; st->tRound = round;
    st->glideLeft = ms;
}

void te_formant_relatch(TaudEngine *e, TaudFormantState *st, const TaudFormantPatch *patch)
{
    st->patch = patch;
    st->sgDue = true;
    formant_glide_to(st, patch->open, patch->front, patch->round);
    if (st->glideLeft > 0) { st->ownVowel = true; st->rejoin = true; }
    else { st->ownVowel = false; st->rejoin = false; }
    if (st->phase == PHASE_ONSET || st->phase == PHASE_VOWEL) {
        const TaudConsonant *cr = consonant(patch->slots[2].cons);
        st->hasCoda = cr != NULL;
        if (cr != NULL) {
            st->coda.row = cr;
            st->coda.slot = patch->slots[2];
            st->coda.next = -1;
            coda_tracks(cr, timing_scale(patch->timing), &st->coda);
        }
    }
}

void te_formant_start_at_vowel(TaudEngine *e, TaudFormantState *st, const TaudFormantPatch *patch)
{
    te_formant_note_on(e, st, patch);
    st->sv = 0;
    st->onsetCount = 0;
    st->seg = 0;
    st->phase = PHASE_VOWEL;
    st->nOn = false;
}

void te_formant_set_vowel(TaudFormantState *st, int f, int g, int r)
{
    st->ownVowel = true;
    st->rejoin = false;
    formant_glide_to(st, (f & 15) * 17, (g & 15) * 17, (r & 15) * 17);
}

static void start_noise(const TaudEngine *e, TaudFormantState *st, const TaudFormantSegment *seg)
{
    const TaudFormantSlot *s = &seg->slot;
    int64_t span, base;
    st->nOn = false;
    if (!consonant_needs_noise(seg->row)) return;
    span = (int64_t)s->len - s->offset;
    base = (int64_t)s->ptr + s->offset;
    if (base + span > SAMPLE_BIN_TOTAL) span = SAMPLE_BIN_TOTAL - base;
    st->nGen = span < 2 || s->rate <= 0;
    st->nOn = true;
    st->nBase = base;
    st->nSpan = (int)span;
    st->nPos = 0.0;
    st->nStep = (double)s->rate / e->rate;
    st->nSeed = NOISE_SEED;
    st->nTilt = s->tilt;
    st->nTiltC = fabs((double)s->tilt) / 128;
    st->nGain = te_atten_gain_of(s->gain);
    st->nZ = 0.0;
}

static double pcm_u8(const TaudEngine *e, int64_t p) { return (te_pool_at(e, p) - 127.5) / 127.5; }

static double read_noise(const TaudEngine *e, TaudFormantState *st)
{
    double x;
    if (st->nGen) {
        uint32_t r = st->nSeed;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        st->nSeed = r;
        x = (st->nSeed / 4294967296.0) * 2 - 1;
    } else {
        int64_t i = (int64_t)st->nPos;
        double fr = st->nPos - (double)i;
        double a = pcm_u8(e, st->nBase + i);
        double b = pcm_u8(e, st->nBase + (i + 1 < st->nSpan ? i + 1 : 0));
        x = a + (b - a) * fr;
        st->nPos += st->nStep;
        while (st->nPos >= st->nSpan) st->nPos -= st->nSpan;
    }
    if (st->nTilt > 0) {
        double y = x - st->nTiltC * st->nZ;
        st->nZ = x;
        x = y;
    } else if (st->nTilt < 0) {
        st->nZ = (1 - st->nTiltC) * x + st->nTiltC * st->nZ;
        x = st->nZ;
    }
    return x * st->nGain;
}

static void set_res(const TaudEngine *e, double *a, int i, double f, double bw)
{
    double T = 1.0 / e->rate;
    double r = js_exp(-PI * bw * T);
    double c = -(r * r);
    double b = 2 * r * js_cos(2 * PI * f * T);
    a[i] = 1 - b - c;
    a[i + 1] = b;
    a[i + 2] = c;
}

static void set_peak_res(const TaudEngine *e, double *a, int i, double f, double bw)
{
    double b, c, w, re, im;
    set_res(e, a, i, f, bw);
    b = a[i + 1]; c = a[i + 2];
    w = (2 * PI * f) / e->rate;
    re = 1 - b * js_cos(w) - c * js_cos(2 * w);
    im = b * js_sin(w) + c * js_sin(2 * w);
    a[i] = sqrt(re * re + im * im);
}

static double bank_norm(const TaudFormantTables *G, const double *bank, const double *g)
{
    double p = 0.0;
    int i, j;
    for (i = 0; i < G->bankN; i++) {
        double re = g[5], im = 0.0;
        for (j = 0; j < 5; j++) {
            double A, b, c, dr, di, k;
            if (g[j] == 0) continue;
            A = bank[j * 3]; b = bank[j * 3 + 1]; c = bank[j * 3 + 2];
            dr = 1 - b * G->bankC1[i] - c * G->bankC2[i];
            di = b * G->bankS1[i] + c * G->bankS2[i];
            k = (g[j] * A) / (dr * dr + di * di);
            re += k * dr;
            im -= k * di;
        }
        p += G->bankW[i] * (re * re + im * im);
    }
    return p > 0 ? sqrt(G->bankRef / p) : 0.0;
}

static double cascade_norm(const TaudEngine *e, const double *co, double f0, const double *w, int wK)
{
    const TaudFormantTables *T = &e->formantTables;
    double R = e->rate, w0, c1, s1, ck, sk, p = 0.0, ref = 0.0, g;
    int K, k, j = 0;
    if (!(f0 > 0)) return 1.0;
    K = (int)dmax(1, dmin(wK, floor((0.45 * R) / f0)));
    w0 = (2 * PI * f0) / R;
    c1 = js_cos(w0); s1 = js_sin(w0);
    ck = c1; sk = s1;
    for (k = 1; k <= K; k++) {
        double c2 = ck * ck - sk * sk, s2 = 2 * sk * ck;
        double h = 1.0, lw, nc;
        int n;
        for (n = 0; n < 7; n++) {
            double A = co[n * 3], b = co[n * 3 + 1], c = co[n * 3 + 2];
            double re = 1 - b * ck - c * c2;
            double im = b * sk + c * s2;
            double gg = (A * A) / (re * re + im * im);
            h *= n == 1 ? 1 / gg : gg;
        }
        lw = w[k] * js_pow(10, elw_db(T, k * f0, &j) / 10);
        p += h * lw;
        ref += lw;
        nc = ck * c1 - sk * s1;
        sk = sk * c1 + ck * s1;
        ck = nc;
    }
    g = p > 0 ? sqrt(ref / p) : NORM_MAX;
    return dmin(dmax(g, NORM_MIN), NORM_MAX);
}

/* Mean and AC power of n bytes from p. */
static void ac_of(const TaudEngine *e, int64_t p, int64_t n, double *mean, double *ac)
{
    double sum = 0.0, sq = 0.0, m;
    int64_t i;
    for (i = 0; i < n; i++) {
        double v = pcm_u8(e, p + i);
        sum += v;
        sq += v * v;
    }
    m = sum / n;
    *mean = m;
    *ac = sq / n - m * m;
}

typedef struct { int64_t at, n; double ac; } Loudest;

static Loudest loudest_window(const TaudEngine *e, int64_t p, int64_t n)
{
    Loudest L;
    double mean;
    int64_t o;
    L.at = p; L.n = n > 0 ? n : 0; L.ac = 0.0;
    if (n < 2) return L;
    if (n <= SOURCE_WINDOW) { ac_of(e, p, n, &mean, &L.ac); return L; }
    L.n = SOURCE_WINDOW;
    L.ac = -1.0;
    for (o = 0; ; o += SOURCE_WINDOW >> 1) {
        int64_t q = o < n - SOURCE_WINDOW ? o : n - SOURCE_WINDOW;
        double ac;
        ac_of(e, p + q, SOURCE_WINDOW, &mean, &ac);
        if (ac > L.ac) { L.ac = ac; L.at = p + q; }
        if (q == n - SOURCE_WINDOW) break;
    }
    return L;
}

static void source_gain(const TaudEngine *e, int64_t p, int64_t n, TaudFormantState *st)
{
    double mean, ac;
    st->sm = 0.0;
    st->sg = 1.0;
    if (n < 2) return;
    ac_of(e, p, n, &mean, &ac);
    st->sm = mean;
    if (!(ac > 1e-12)) return;
    st->sg = dmin(dmax(FORMANT_LEVEL / sqrt(ac), 1.0 / SOURCE_GAIN_MAX), SOURCE_GAIN_MAX);
}

static void saw_harmonics(TaudFormantState *st)
{
    int k;
    for (k = 1; k <= NORM_HARMONICS; k++) st->hw[k] = 1.0 / (k * k);
    st->hwK = NORM_HARMONICS;
}

static void source_harmonics(TaudEngine *e, int64_t p, int64_t span, bool whole, double P, TaudFormantState *st)
{
    double *w = st->hw, *x = e->formantTables.dft, mean = 0.0, sum = 0.0;
    int K = P >= 2 ? (int)dmin(NORM_HARMONICS, floor(P / 2)) : 0;
    int64_t N = span < SOURCE_DFT_MAX ? span : SOURCE_DFT_MAX, i;
    bool exact;
    double m;
    int k;
    TaudFormantTables *T = &e->formantTables;
    uint32_t h = 0x811c9dc5u;
    int j;
    if (K < 1 || N < 2) { saw_harmonics(st); return; }
    exact = whole && span <= SOURCE_DFT_MAX;
    m = exact ? dmax(1, js_round(N / P)) : 0;
    /* A pure memo, keyed as the JS keys it, so a chord pays for one. */
    for (i = 0; i < N; i++) h = (h ^ (uint32_t)te_pool_at(e, p + i)) * 16777619u;
    for (j = 0; j < T->memoCount; j++) {
        if (T->memo[j].p == p && T->memo[j].N == N && T->memo[j].m == m && T->memo[j].P == P && T->memo[j].h == h) {
            memcpy(w, T->memo[j].w, (size_t)(T->memo[j].K + 1) * sizeof *w);
            st->hwK = T->memo[j].K;
            return;
        }
    }
    for (i = 0; i < N; i++) mean += pcm_u8(e, p + i);
    mean /= N;
    for (i = 0; i < N; i++) {
        double v = pcm_u8(e, p + i) - mean;
        x[i] = exact ? v : v * (0.5 - 0.5 * js_cos((2 * PI * i) / N));
    }
    for (k = 1; k <= K; k++) {
        double om = exact ? (2 * PI * k * m) / N : (2 * PI * k) / P;
        double c1 = js_cos(om), s1 = js_sin(om);
        double c = 1.0, s = 0.0, re = 0.0, im = 0.0;
        for (i = 0; i < N; i++) {
            double nc;
            re += x[i] * c;
            im -= x[i] * s;
            nc = c * c1 - s * s1;
            s = s * c1 + c * s1;
            c = nc;
        }
        w[k] = re * re + im * im;
        sum += w[k];
    }
    if (!(sum > 0)) { saw_harmonics(st); return; }
    st->hwK = K;
    if (T->memoCount >= 32) T->memoCount = 0;
    T->memo[T->memoCount].p = p;
    T->memo[T->memoCount].N = N;
    T->memo[T->memoCount].m = m;
    T->memo[T->memoCount].P = P;
    T->memo[T->memoCount].h = h;
    T->memo[T->memoCount].K = K;
    memcpy(T->memo[T->memoCount].w, w, (size_t)(K + 1) * sizeof *w);
    T->memoCount++;
}

static void measure_source(TaudEngine *e, const TaudVoice *voice, TaudFormantState *st)
{
    int mode = voice->activeLoopMode & 3;
    bool looped = (mode == 1 || mode == 2) && voice->activeSampleLoopEnd > voice->activeSampleLoopStart;
    int64_t base = voice->activeSamplePtr;
    int64_t lim = SAMPLE_BIN_TOTAL - base;
    int64_t p0 = voice->activeSamplePlayStart, at, n;
    bool whole = false;
#define CUT(x) ((int64_t)dmax(0, dmin(dmin((double)(x), voice->activeSampleLength), (double)lim)))
    if (looped) {
        int64_t a = voice->activeSampleLoopStart, preN;
        at = base + a;
        n = CUT(voice->activeSampleLoopEnd) - a;
        whole = n == voice->activeSampleLoopEnd - a;
        preN = CUT(a) - p0;
        if (preN > SOURCE_MEASURE_MAX) preN = SOURCE_MEASURE_MAX;
        if (preN >= 2 && n >= 2) {
            double mean, loopAc;
            Loudest w;
            ac_of(e, at, n < SOURCE_MEASURE_MAX ? n : SOURCE_MEASURE_MAX, &mean, &loopAc);
            w = loudest_window(e, base + p0, preN);
            if (w.ac > loopAc) { at = w.at; n = w.n; whole = false; }
        }
    } else {
        int64_t len = CUT(voice->activeSampleLength) - p0;
        Loudest w = loudest_window(e, base + p0, len < SOURCE_MEASURE_MAX ? len : SOURCE_MEASURE_MAX);
        at = w.at; n = w.n;
    }
#undef CUT
    source_gain(e, at, n < SOURCE_MEASURE_MAX ? n : SOURCE_MEASURE_MAX, st);
    source_harmonics(e, at, n, whole, voice->activeSamplingRate / LINEAR_FREQ_C4_HZ, st);
}

static double voice_f0(const TaudEngine *e, const TaudVoice *voice)
{
    int rate = voice->activeSamplingRate;
    double pr = voice->snapPlaybackRate ? voice->playbackRate : voice->currentPlaybackRate;
    return rate > 0 ? (pr * e->rate * LINEAR_FREQ_C4_HZ) / rate : 0.0;
}

static void seg_amps(const TaudFormantSegment *seg, double t, double *out)
{
    out[0] = track_at(&seg->av, t);
    out[1] = track_at(&seg->ah, t);
    out[2] = track_at(&seg->af, t);
}

static void blend_into(const TaudConsonant *row, double w, double s, double *F, double *B,
                       const double *NF, const double *NB)
{
    int n;
    for (n = 0; n < 3; n++) {
        double cf = row->F[n] > 0 ? row->F[n] * s : NF[n];
        double cb = row->B[n] > 0 ? row->B[n] * s : NB[n];
        F[n] = NF[n] + w * (cf - NF[n]);
        B[n] = NB[n] + w * (cb - NB[n]);
    }
    F[3] = NF[3]; F[4] = NF[4]; B[3] = NB[3]; B[4] = NB[4];
}

/* One control update (≈ 1 ms): the sequencer, the vowel, the coefficients. */
static void formant_control(TaudEngine *e, TaudVoice *voice, TaudFormantState *st)
{
    int upd = st->upd, n;
    double dt = (upd * 1000.0) / e->rate;
    const TaudFormantPatch *patch = st->patch;
    double f0, ref, s, nzAmt = 0.0, ceil, fnp, fnz, nvEnd, inv;
    double *V = st->V, *VB = st->VB, *F = st->F, *B = st->B, *casc = st->casc;
    double ampNow[3], ampNext[3], nextF[5], nextB[5];
    const TaudFormantSegment *seg = NULL;
    st->ctl = upd;

    if (voice->noteFading && st->phase != PHASE_CODA) st->codaVeto = true;
    while (st->phase == PHASE_ONSET && st->t >= st->onset[st->seg].end) {
        st->t -= st->onset[st->seg].end;
        st->seg++;
        if (st->seg >= st->onsetCount) { st->phase = PHASE_VOWEL; st->nOn = false; }
        else start_noise(e, st, &st->onset[st->seg]);
    }
    if (st->phase == PHASE_VOWEL && voice->keyOff && !st->codaVeto && st->hasCoda) {
        st->phase = PHASE_CODA;
        st->t = 0.0;
        start_noise(e, st, &st->coda);
    }
    if (st->phase == PHASE_CODA && st->t >= st->coda.end) {
        st->phase = PHASE_DONE;
        st->av = 0; st->avStep = 0; st->ah = 0; st->ahStep = 0; st->af = 0; st->afStep = 0;
        st->nOn = false;
        te_start_ramp_out(e, voice);
        return;
    }

    if (st->glideLeft > 0) {
        if (st->glideLeft <= dt) {
            st->vOpen = st->tOpen; st->vFront = st->tFront; st->vRound = st->tRound;
            st->glideLeft = 0.0;
            if (st->rejoin) { st->ownVowel = false; st->rejoin = false; }
        } else {
            st->vOpen += st->gOpen * dt;
            st->vFront += st->gFront * dt;
            st->vRound += st->gRound * dt;
            st->glideLeft -= dt;
        }
    } else if (!st->ownVowel) {
        st->vOpen = patch->open; st->vFront = patch->front; st->vRound = patch->round;
    }
    f0 = voice_f0(e, voice);
    ref = note_hz(patch->refNote);
    s = vowel_at_pitch(e, st->vFront, st->vOpen, st->vRound, f0, ref, patch->track, V, VB, patch->rhotic);
    if (st->sv != 0) {
        double gw = track_at(&st->svW, st->svT);
        if (gw > 0) {
            double *VG = st->VG, *VGB = st->VGB;
            vowel_at_pitch(e, SEMIVOWELS[st->sv].front, SEMIVOWELS[st->sv].open, SEMIVOWELS[st->sv].round,
                           f0, ref, patch->track, VG, VGB, false);
            for (n = 0; n < 5; n++) {
                V[n] *= js_pow(VG[n] / V[n], gw);
                VB[n] *= js_pow(VGB[n] / VB[n], gw);
            }
        }
    }

    if (st->phase == PHASE_ONSET) {
        seg = &st->onset[st->seg];
        if (seg->next >= 0) {
            const TaudFormantSegment *nx = &st->onset[seg->next];
            blend_into(nx->row, nx->w.p[1], s, nextF, nextB, V, VB);
        } else {
            memcpy(nextF, V, sizeof nextF);
            memcpy(nextB, VB, sizeof nextB);
        }
        blend_into(seg->row, track_at(&seg->w, st->t), s, F, B, nextF, nextB);
        nzAmt = track_at(&seg->nz, st->t);
        seg_amps(seg, st->t, ampNow);
        seg_amps(seg, st->t + dt, ampNext);
    } else if (st->phase == PHASE_CODA) {
        seg = &st->coda;
        blend_into(seg->row, track_at(&seg->w, st->t), s, F, B, V, VB);
        nzAmt = track_at(&seg->nz, st->t);
        seg_amps(seg, st->t, ampNow);
        seg_amps(seg, st->t + dt, ampNext);
    } else {
        bool gl = st->sv != 0;
        memcpy(F, V, 5 * sizeof *F);
        memcpy(B, VB, 5 * sizeof *B);
        ampNow[0] = gl ? track_at(&st->svAv, st->svT) : 1; ampNow[1] = 0; ampNow[2] = 0;
        ampNext[0] = gl ? track_at(&st->svAv, st->svT + dt) : 1; ampNext[1] = 0; ampNext[2] = 0;
    }
    ceil = 0.45 * e->rate;
    fnp = dmin(FNP * s, ceil);
    fnz = seg != NULL && seg->row->fnz > 0 ? dmin(seg->row->fnz * s, ceil) : fnp;
    set_res(e, casc, 0, fnp, BNP * s);
    set_res(e, casc, 3, fnp + nzAmt * (fnz - fnp), BNZ * s);
    for (n = 0; n < 5; n++) set_res(e, casc, 6 + (4 - n) * 3, dmin(F[n], ceil), dmin(B[n], ceil));
    if (seg != NULL && st->nOn) {
        double *bank = st->bank, *g = st->bankGain;
        const double *a = seg->row->bank;
        int j;
        set_peak_res(e, bank, 0, dmin(F[1], ceil), dmin(B[1], ceil));
        set_peak_res(e, bank, 3, dmin(F[2], ceil), dmin(B[2], ceil));
        for (j = 0; j < 3; j++) {
            set_peak_res(e, bank, 6 + j * 3, dmin(PAR_F[j] * s, ceil), dmin(PAR_B[j] * s, ceil));
        }
        for (j = 0; j < 5; j++) {
            double gj = a[j] > 0 ? db_gain(a[j] - 60) : 0;
            g[j] = (j & 1) == 0 ? gj : -gj;
        }
        g[5] = a[5] > 0 ? db_gain(a[5] - 60) : 0;
        st->nb = bank_norm(&e->formantTables, bank, g);
    }

    if (st->fresh || st->normCount <= 0 || st->phase != PHASE_VOWEL) {
        st->nvGoal = cascade_norm(e, casc, f0, st->hw, st->hwK);
        st->normCount = NORM_EVERY;
    }
    st->normCount--;
    nvEnd = st->fresh || (st->phase != PHASE_VOWEL && st->nvGoal < st->nv)
        ? st->nvGoal : st->nv + (st->nvGoal - st->nv) * 0.25;
    if (st->fresh) st->nv = nvEnd;

    inv = 1.0 / upd;
    st->av = ampNow[0]; st->avStep = (ampNext[0] - ampNow[0]) * inv;
    st->ah = ampNow[1]; st->ahStep = (ampNext[1] - ampNow[1]) * inv;
    st->af = ampNow[2]; st->afStep = (ampNext[2] - ampNow[2]) * inv;
    st->nvStep = (nvEnd - st->nv) * inv;
    st->fresh = false;
    if (st->phase != PHASE_VOWEL) st->t += dt;
    if (st->sv != 0 && st->phase != PHASE_ONSET) {
        st->svT += dt;
        if (st->svT >= st->svEnd) st->sv = 0;
    }
}

static double tract_res(const double *co, int i, double *cy, int j, double x)
{
    double y = co[i] * x + co[i + 1] * cy[j] + co[i + 2] * cy[j + 1];
    cy[j + 1] = cy[j];
    cy[j] = y;
    return y;
}

double te_render_formant_voice(TaudEngine *e, TaudVoice *voice, TaudInst *inst, int interpMode)
{
    TaudFormantState *st = voice->formant;
    double g, n, x, out;
    double *casc = st->casc, *cy = st->cy;
    if (st->sgDue) { measure_source(e, voice, st); st->sgDue = false; }
    g = te_fetch_tracker_sample(e, voice, inst, interpMode, 0);
    if (st->phase == PHASE_DONE) return 0.0;
    if (st->ctl <= 0) {
        formant_control(e, voice, st);
        if (st->phase == PHASE_DONE) return 0.0;
    }
    st->ctl--;
    n = st->nOn ? read_noise(e, st) : 0.0;
    x = st->av * st->sg * (g - st->sm) + st->ah * n;
    x = tract_res(casc, 0, cy, 0, x);
    {
        double A = casc[3], b = casc[4], c = casc[5];
        double y = (x - b * cy[2] - c * cy[3]) / A;
        cy[3] = cy[2];
        cy[2] = x;
        x = y;
    }
    x = tract_res(casc, 6, cy, 4, x);
    x = tract_res(casc, 9, cy, 6, x);
    x = tract_res(casc, 12, cy, 8, x);
    x = tract_res(casc, 15, cy, 10, x);
    x = tract_res(casc, 18, cy, 12, x);
    out = st->nv * x;
    if (st->nOn) {
        double *bank = st->bank, *by = st->by, *gb = st->bankGain;
        double f = gb[5] * n;
        int j;
        for (j = 0; j < 5; j++) {
            int i = j * 3;
            double y = bank[i] * n + bank[i + 1] * by[j * 2] + bank[i + 2] * by[j * 2 + 1];
            by[j * 2 + 1] = by[j * 2];
            by[j * 2] = y;
            f += gb[j] * y;
        }
        out += st->af * st->nb * f;
    }
    st->av += st->avStep;
    st->ah += st->ahStep;
    st->af += st->afStep;
    st->nv += st->nvStep;
    return out;
}
