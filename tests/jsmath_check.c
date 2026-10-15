/* Checks src/jsmath.c against V8, bit for bit (tests/jsmath_ref.mjs). */
#include <stdio.h>
#include <string.h>
#include "../src/taud_internal.h"

static const char *NAMES[] = { "sin", "cos", "tan", "exp", "log10", "log2", "acos", "asin", "atan2", "log", "pow" };
int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "tests/out/jsmath.bin", "rb");
    double r[4];
    long total[11] = {0}, bad[11] = {0};
    int i, fail = 0;
    if (f == NULL) return 2;
    while (fread(r, 8, 4, f) == 4) {
        int id = (int)r[0];
        double v;
        switch (id) {
        case 0: v = js_sin(r[1]); break;
        case 1: v = js_cos(r[1]); break;
        case 2: v = js_tan(r[1]); break;
        case 3: v = js_exp(r[1]); break;
        case 4: v = js_log10(r[1]); break;
        case 5: v = js_log2(r[1]); break;
        case 6: v = js_acos(r[1]); break;
        case 7: v = js_asin(r[1]); break;
        case 8: v = js_atan2(r[1], r[2]); break;
        case 9: v = js_log(r[1]); break;
        default: v = js_pow(r[1], r[2]); break;  /* whichever the engine uses */
        }
        total[id]++;
        if (memcmp(&v, &r[3], 8) != 0 && !(v != v && r[3] != r[3])) {
            if (bad[id] < 3) printf("  %s(%.17g, %.17g): got %.17g want %.17g\n", NAMES[id], r[1], r[2], v, r[3]);
            bad[id]++;
        }
    }
    for (i = 0; i < 11; i++) {
        printf("%-6s %9ld args, %ld mismatches\n", NAMES[i], total[i], bad[i]);
        if (bad[i]) fail = 1;
    }
    return fail;
}
