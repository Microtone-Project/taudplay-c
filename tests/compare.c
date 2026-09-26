/* Compare two interleaved stereo float32 renders: exact-match count, the
 * largest difference, and the first frame that differs. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    float *buf;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (float *)malloc((size_t)sz + 4);
    if (buf == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *n = (size_t)sz / sizeof(float);
    return buf;
}

int main(int argc, char **argv)
{
    size_t na, nb, n, i, same = 0, first = (size_t)-1, worst = 0;
    double maxd = 0, sumsq = 0, refsq = 0;
    float *a, *b;
    if (argc < 3) { fprintf(stderr, "usage: compare REF.f32 TEST.f32\n"); return 2; }
    a = load(argv[1], &na);
    b = load(argv[2], &nb);
    if (a == NULL || b == NULL) { fprintf(stderr, "compare: cannot read inputs\n"); return 2; }
    n = na < nb ? na : nb;
    for (i = 0; i < n; i++) {
        double d = fabs((double)a[i] - (double)b[i]);
        if (memcmp(&a[i], &b[i], sizeof(float)) == 0) same++;
        else if (first == (size_t)-1) first = i;
        if (d > maxd) { maxd = d; worst = i; }
        sumsq += d * d;
        refsq += (double)a[i] * a[i];
    }
    printf("  samples %zu vs %zu, bit-identical %.4f%%, max |diff| %.3g (frame %zu)",
           na, nb, n ? 100.0 * same / n : 100.0, maxd, worst / 2);
    if (first != (size_t)-1) printf(", first diff at frame %zu", first / 2);
    if (sumsq > 0) printf(", SNR %.1f dB", 10 * log10(refsq / sumsq));
    printf("\n");
    return (na == nb && same == n) ? 0 : 1;
}
