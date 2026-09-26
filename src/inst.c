/* Taud instrument data model — port of inst.js. */

#include <stdlib.h>
#include <string.h>

#include "taud_internal.h"

static void make_env(TaudEnvPoint *env, int defaultValue)
{
    int i;
    for (i = 0; i < ENV_POINTS; i++) { env[i].value = (uint8_t)defaultValue; env[i].offset = 0; }
}

void te_inst_init(TaudInst *inst, int index)
{
    memset(inst, 0, sizeof *inst);
    inst->index = index;
    inst->instGlobalVolume = 0xff;
    make_env(inst->volEnvelopes, 0x3f);
    make_env(inst->panEnvelopes, 0x80);
    make_env(inst->pfEnvelopes, 0x80);
    make_env(inst->pf2Envelopes, 0x80);
    inst->defaultPan = 0x80;
    inst->pitchPanCentre = 0x5000;
    inst->defaultCutoff = 0xff;
    inst->cutoffOverride = -1;
    inst->resonanceOverride = -1;
    inst->modTo = 1;
    inst->modCombBits = -1;
    inst->modExtSwapA = -1;
    inst->modExtSwapB = -1;
    inst->modFunkWalk = -1;
    inst->modFunkPos = -1;
}

void te_inst_free(TaudInst *inst)
{
    free(inst->extraPatches);
    free(inst->meta);
    free(inst->invertMask);
    free(inst->modMask);
    inst->extraPatches = NULL;
    inst->patchCount = 0;
    inst->meta = NULL;
    inst->invertMask = NULL;
    inst->modMask = NULL;
}

static void make_patch(TaudPatch *p)
{
    memset(p, 0, sizeof *p);
    p->defaultPan = 0xff;
    p->vibratoWaveform = 0xff;
    p->extraCutoff = 0xff;
    p->extraResonance = 0xff;
    p->chanCount = 1;
    p->chanMode = CHAN_MODE_DISCRETE;
}

bool te_patch_is_stereo(const TaudPatch *p)
{
    return p->hasChanBlock && p->chanCount == 2 && p->chanPtrCount >= 1;
}

bool te_patch_vibrato_inherits(const TaudPatch *p)
{
    return p->vibratoWaveform == 0xff && p->vibratoSpeed == 0 &&
        p->vibratoSweep == 0 && p->vibratoDepth == 0 && p->vibratoRate == 0;
}

const TaudPatch *te_patch_at(const TaudInst *inst, int patchIndex)
{
    if (inst == NULL || patchIndex < 0) return NULL;
    return inst->extraPatches != NULL && patchIndex < inst->patchCount
        ? &inst->extraPatches[patchIndex] : NULL;
}

/* Parse a flat variable-length Ixmp patch blob. Returns the patch count (0 for
 * a short or empty blob) with a malloc'd array in *out, or -1 on OOM. */
int te_parse_patches_blob(const uint8_t *bytes, size_t len, TaudPatch **out)
{
    size_t o = 0;
    int n = 0, cap = 0;
    TaudPatch *patches = NULL;
    *out = NULL;
    if (len < 31) return 0;

#define U8(x) ((int)bytes[(x)])
#define U16(x) ((int)bytes[(x)] | ((int)bytes[(x) + 1] << 8))
#define U32(x) ((uint32_t)bytes[(x)] | ((uint32_t)bytes[(x) + 1] << 8) | \
                ((uint32_t)bytes[(x) + 2] << 16) | ((uint32_t)bytes[(x) + 3] << 24))

    while (o + 31 <= len) {
        int ver = U8(o);
        size_t p = o + 31;
        TaudPatch t;
        make_patch(&t);
        if ((ver & 0x80) != 0) { /* 'x' block (15 bytes) */
            if (p + 15 > len) break;
            t.filterSfMode = (U8(p) & 0x01) != 0;
            t.fadeoutStep = U16(p + 8);
            t.extraCutoff = U16(p + 10);
            t.extraResonance = U16(p + 12);
            t.extraInitialAttenOctet = U8(p + 14);
            t.hasExtra = true;
            p += 15;
        }
#define READ_ENV(has, arr, loopF, susF)                                   \
        do {                                                              \
            int k;                                                        \
            if (p + 54 > len) goto done;                                  \
            t.loopF = U16(p);                                             \
            t.susF = U16(p + 2);                                          \
            for (k = 0; k < 25; k++) {                                    \
                t.arr[k].value = bytes[p + 4 + 2 * k];                    \
                t.arr[k].offset = bytes[p + 5 + 2 * k];                   \
            }                                                             \
            t.has = true;                                                 \
            p += 54;                                                      \
        } while (0)
        if ((ver & 0x02) != 0) READ_ENV(hasVolEnv, volEnv, volEnvLoop, volEnvSustain);
        if ((ver & 0x04) != 0) READ_ENV(hasPanEnv, panEnv, panEnvLoop, panEnvSustain);
        if ((ver & 0x08) != 0) READ_ENV(hasFilterEnv, filterEnv, filterEnvLoop, filterEnvSustain);
        if ((ver & 0x10) != 0) READ_ENV(hasPitchEnv, pitchEnv, pitchEnvLoop, pitchEnvSustain);
#undef READ_ENV
        if ((ver & 0x20) != 0) {
            int cb, k;
            if (p + 4 > len) break;
            cb = U8(p);
            t.chanCount = (cb >> 4) + 1;
            t.chanMode = cb & 0x0f;
            t.chanFlags = U8(p + 1) | (U8(p + 2) << 8) | (U8(p + 3) << 16);
            if (p + 4 + 4 * (size_t)(t.chanCount - 1) > len) break;
            for (k = 0; k < t.chanCount - 1; k++) t.chanPtrs[k] = U32(p + 4 + 4 * k);
            t.chanPtrCount = t.chanCount - 1;
            t.hasChanBlock = true;
            p += 4 + 4 * (size_t)(t.chanCount - 1);
        }
        t.pitchStart = U16(o + 1);
        t.pitchEnd = U16(o + 3);
        t.volumeStart = U8(o + 5);
        t.volumeEnd = U8(o + 6);
        t.samplePtr = U32(o + 7);
        t.sampleLength = U16(o + 11);
        t.playStart = U16(o + 13);
        t.loopStart = U16(o + 15);
        t.loopEnd = U16(o + 17);
        t.samplingRate = U16(o + 19);
        { int v = U16(o + 21); t.sampleDetune = v >= 0x8000 ? v - 0x10000 : v; }
        t.loopMode = U8(o + 23);
        t.defaultPan = U8(o + 24);
        t.defaultNoteVolume = U8(o + 25);
        t.vibratoSpeed = U8(o + 26);
        t.vibratoSweep = U8(o + 27);
        t.vibratoDepth = U8(o + 28);
        t.vibratoRate = U8(o + 29);
        t.vibratoWaveform = U8(o + 30);
        if (n == cap) {
            int nc = cap ? cap * 2 : 8;
            TaudPatch *np = (TaudPatch *)realloc(patches, (size_t)nc * sizeof *np);
            if (np == NULL) { free(patches); return -1; }
            patches = np;
            cap = nc;
        }
        patches[n++] = t;
        o = p;
    }
done:
#undef U8
#undef U16
#undef U32
    if (n == 0) { free(patches); return 0; }
    *out = patches;
    return n;
}

const TaudPatch *te_inst_resolve_patch(const TaudInst *inst, int noteVal, int rowVolume)
{
    int i;
    if (inst->extraPatches == NULL) return NULL;
    for (i = 0; i < inst->patchCount; i++) {
        const TaudPatch *p = &inst->extraPatches[i];
        if (noteVal >= p->pitchStart && noteVal <= p->pitchEnd &&
            rowVolume >= p->volumeStart && rowVolume <= p->volumeEnd) return p;
    }
    return NULL;
}

/* All meta layers whose rectangle contains the trigger, in record order.
 * Writes pointers into out[] (capacity META_MAX_LAYERS), returns the count. */
int te_inst_resolve_meta_layers(const TaudInst *inst, int noteVal, int rowVolume, const TaudMetaLayer **out)
{
    int i, n = 0;
    if (inst->meta == NULL) return 0;
    for (i = 0; i < inst->meta->count; i++) {
        const TaudMetaLayer *l = &inst->meta->layers[i];
        if (noteVal >= l->pitchStart && noteVal <= l->pitchEnd &&
            rowVolume >= l->volStart && rowVolume <= l->volEnd) out[n++] = l;
    }
    return n;
}

/* ── FM program (§7.6) ── */

typedef struct { int pop, push; } Arity;

static bool fm_word_arity(int word, int opCount, Arity *a)
{
    int w = word & 0xffff, idx;
    if (w >= FM_WORD_OP) {
        switch (w) {
        case FMOP_ADD: case FMOP_MUL: a->pop = 2; a->push = 1; return true;
        case FMOP_NEG: a->pop = 1; a->push = 1; return true;
        case FMOP_DUP: a->pop = 1; a->push = 2; return true;
        case FMOP_SWAP: a->pop = 2; a->push = 2; return true;
        default: return false;
        }
    }
    idx = w & FM_INDEX_MASK;
    if (idx >= opCount) return false;
    switch (w & ~FM_INDEX_MASK) {
    case FM_WORD_OSC: case FM_WORD_FB: a->pop = 0; a->push = 1; return true;
    case FM_WORD_MOD: a->pop = 1; a->push = 1; return true;
    default: return false;
    }
}

/* Read the RPN program at byte `off`; false when it does not parse. */
static bool decode_fm_program(const uint8_t *b, int off, int opCount, TaudMeta *m)
{
    int o, depth = 0, n = 0;
    bool ended = false;
    for (o = off; o + 2 <= 256; o += 2) {
        int w = b[o] | (b[o + 1] << 8);
        Arity a;
        if (w == FMOP_END) { ended = true; break; }
        if (!fm_word_arity(w, opCount, &a)) return false;
        if (depth < a.pop) return false;
        depth += a.push - a.pop;
        if (depth > FM_STACK_MAX) return false;
        m->program[n++] = (uint16_t)w;
    }
    if (!ended && off + n * 2 + 2 <= 256) return false;
    if (depth < 1) return false;
    m->programLen = n;
    return true;
}

/* ── the 256-byte record ── */

static void env_point_set(TaudEnvPoint *env, int base, int offset, int byte)
{
    int rel = offset - base;
    TaudEnvPoint *pt = &env[rel >> 1];
    if ((rel & 1) == 0) pt->value = (uint8_t)byte;
    else pt->offset = (uint8_t)byte;
}

static void inst_set_byte(TaudInst *t, int o, int byte)
{
    if (o >= 21 && o <= 70) { env_point_set(t->volEnvelopes, 21, o, byte); return; }
    if (o >= 71 && o <= 120) { env_point_set(t->panEnvelopes, 71, o, byte); return; }
    if (o >= 121 && o <= 170) { env_point_set(t->pfEnvelopes, 121, o, byte); return; }
    if (o >= 201 && o <= 250) { env_point_set(t->pf2Envelopes, 201, o, byte); return; }
    if (o >= 252 && o <= 255) { t->reserved[o - 251] = (uint8_t)byte; return; }
    switch (o) {
    case 0: t->samplePtr = (t->samplePtr & 0xffffff00u) | (uint32_t)byte; break;
    case 1: t->samplePtr = (t->samplePtr & 0xffff00ffu) | ((uint32_t)byte << 8); break;
    case 2: t->samplePtr = (t->samplePtr & 0xff00ffffu) | ((uint32_t)byte << 16); break;
    case 3: t->samplePtr = (t->samplePtr & 0x00ffffffu) | ((uint32_t)byte << 24); break;
    case 4: t->sampleLength = (t->sampleLength & 0xff00) | byte; break;
    case 5: t->sampleLength = (t->sampleLength & 0x00ff) | (byte << 8); break;
    case 6: t->samplingRate = (t->samplingRate & 0xff00) | byte; break;
    case 7: t->samplingRate = (t->samplingRate & 0x00ff) | (byte << 8); break;
    case 8: t->samplePlayStart = (t->samplePlayStart & 0xff00) | byte; break;
    case 9: t->samplePlayStart = (t->samplePlayStart & 0x00ff) | (byte << 8); break;
    case 10: t->sampleLoopStart = (t->sampleLoopStart & 0xff00) | byte; break;
    case 11: t->sampleLoopStart = (t->sampleLoopStart & 0x00ff) | (byte << 8); break;
    case 12: t->sampleLoopEnd = (t->sampleLoopEnd & 0xff00) | byte; break;
    case 13: t->sampleLoopEnd = (t->sampleLoopEnd & 0x00ff) | (byte << 8); break;
    case 14: t->loopMode = byte & 0x37; break;
    case 15: t->volEnvLoop = (t->volEnvLoop & 0xff00) | byte; break;
    case 16: t->volEnvLoop = (t->volEnvLoop & 0x00ff) | (byte << 8); break;
    case 17: t->panEnvLoop = (t->panEnvLoop & 0xff00) | byte; break;
    case 18: t->panEnvLoop = (t->panEnvLoop & 0x00ff) | (byte << 8); break;
    case 19: t->pfEnvLoop = (t->pfEnvLoop & 0xff00) | byte; break;
    case 20: t->pfEnvLoop = (t->pfEnvLoop & 0x00ff) | (byte << 8); break;
    case 171: t->instGlobalVolume = byte & 0xff; break;
    case 172: t->volumeFadeoutLow = byte & 0xff; break;
    case 173: t->fadeoutHigh = byte & 0x1f; break;
    case 174: t->volumeSwing = byte & 0xff; break;
    case 175: t->vibratoSpeed = byte & 0xff; break;
    case 176: t->vibratoSweep = byte & 0xff; break;
    case 177: t->defaultPan = byte & 0xff; break;
    case 178: t->pitchPanCentre = (t->pitchPanCentre & 0xff00) | byte; break;
    case 179: t->pitchPanCentre = (t->pitchPanCentre & 0x00ff) | (byte << 8); break;
    case 180: t->pitchPanSeparation = byte >= 0x80 ? byte - 0x100 : byte; break;
    case 181: t->panSwing = byte & 0xff; break;
    case 182: t->defaultCutoff = byte & 0xff; break;
    case 183: t->defaultResonance = byte & 0xff; break;
    case 184: t->sampleDetune = (t->sampleDetune & 0xff00) | byte; break;
    case 185: t->sampleDetune = (t->sampleDetune & 0x00ff) | (byte << 8); break;
    case 186: t->instrumentFlag = byte & 0xff; break;
    case 187: t->vibratoDepth = byte & 0xff; break;
    case 188: t->vibratoRate = byte & 0xff; break;
    case 189: t->volEnvSustainWord = (t->volEnvSustainWord & 0xff00) | byte; break;
    case 190: t->volEnvSustainWord = (t->volEnvSustainWord & 0x00ff) | (byte << 8); break;
    case 191: t->panEnvSustainWord = (t->panEnvSustainWord & 0xff00) | byte; break;
    case 192: t->panEnvSustainWord = (t->panEnvSustainWord & 0x00ff) | (byte << 8); break;
    case 193: t->pfEnvSustainWord = (t->pfEnvSustainWord & 0xff00) | byte; break;
    case 194: t->pfEnvSustainWord = (t->pfEnvSustainWord & 0x00ff) | (byte << 8); break;
    case 195: t->dupCheckFlag = byte & 0x0f; break;
    case 196: t->defaultNoteVolume = byte & 0xff; break;
    case 197: t->pf2EnvLoop = (t->pf2EnvLoop & 0xff00) | byte; break;
    case 198: t->pf2EnvLoop = (t->pf2EnvLoop & 0x00ff) | (byte << 8); break;
    case 199: t->pf2EnvSustainWord = (t->pf2EnvSustainWord & 0xff00) | byte; break;
    case 200: t->pf2EnvSustainWord = (t->pf2EnvSustainWord & 0x00ff) | (byte << 8); break;
    case 251: t->initialAttenOctet = byte & 0xff; break;
    default: break;
    }
}

/** Load a full 256-byte record; detects the Metainstrument sentinel. */
void te_inst_load_record(TaudInst *inst, const uint8_t *b)
{
    uint32_t sp = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    inst->cutoffOverride = -1;
    inst->resonanceOverride = -1;
    if (((sp >> 16) & 0xffff) == 0xffff) {
        int type = (b[0] >> 4) & 0x0f;
        bool fm = type == META_TYPE_FM;
        int rawCount = (sp >> 8) & 0xff;
        int count = fm ? imin(rawCount, FM_MAX_OPERATORS) : rawCount;
        int o = 4, n;
        TaudMeta m;
        memset(&m, 0, sizeof m);
        for (n = 0; n < count; n++) {
            int instIdx, mixOctet, detRaw, detune, pStart, pEnd, vStart, vEnd;
            bool fixedPitch, usable;
            if (o + 10 > 256) break;
            instIdx = b[o] | (((b[o + 8] >> 6) & 0x3) << 8);
            mixOctet = b[o + 1];
            detRaw = b[o + 2] | (b[o + 3] << 8);
            fixedPitch = !fm && (b[o + 9] & META_LAYER_FIXED_PITCH) != 0;
            detune = fixedPitch ? detRaw : (detRaw >= 0x8000 ? detRaw - 0x10000 : detRaw);
            pStart = b[o + 4] | (b[o + 5] << 8);
            pEnd = b[o + 6] | (b[o + 7] << 8);
            vStart = b[o + 8] & 0x3f;
            vEnd = b[o + 9] & 0x3f;
            usable = instIdx >= 1 && instIdx <= 1023 && instIdx != inst->index;
            if ((usable || fm) && m.count < META_MAX_LAYERS) {
                TaudMetaLayer *l = &m.layers[m.count++];
                l->instIdx = usable ? instIdx : 0;
                l->mixOctet = mixOctet;
                l->detune = detune;
                l->pitchStart = pStart;
                l->pitchEnd = pEnd;
                l->volStart = vStart;
                l->volEnd = vEnd;
                l->fixedPitch = fixedPitch;
            }
            o += 10;
        }
        free(inst->meta);
        inst->meta = NULL;
        if (m.count > 0) {
            m.flags = b[0];
            m.strict = (b[0] & 0x01) != 0;
            m.type = type;
            m.hasProgram = fm ? decode_fm_program(b, o, m.count, &m) : false;
            inst->meta = (TaudMeta *)malloc(sizeof m);
            if (inst->meta != NULL) *inst->meta = m;
        }
        free(inst->extraPatches);
        inst->extraPatches = NULL;
        inst->patchCount = 0;
    } else {
        int i;
        free(inst->meta);
        inst->meta = NULL;
        for (i = 0; i < 256; i++) inst_set_byte(inst, i, b[i]);
    }
}

/* ── invert-loop mask (S $F0xx) ── */

static uint8_t *ensure_mask(uint8_t **buf)
{
    if (*buf == NULL) *buf = (uint8_t *)malloc(MOD_MASK_CAP);
    return *buf;
}

void te_inst_toggle_invert_bit(TaudInst *inst, int loopOffset, int loopLen)
{
    int len = imax(loopLen, 1);
    int expectedSize = (len + 7) >> 3;
    int idx;
    if (expectedSize > MOD_MASK_CAP) expectedSize = MOD_MASK_CAP;
    if (inst->invertMaskLen == 0 || inst->invertMaskLen != expectedSize) {
        if (ensure_mask(&inst->invertMask) == NULL) return;
        memset(inst->invertMask, 0, (size_t)expectedSize);
        inst->invertMaskLen = expectedSize;
    }
    idx = iclamp(loopOffset, 0, len - 1);
    if ((idx >> 3) < inst->invertMaskLen) inst->invertMask[idx >> 3] ^= (uint8_t)(1 << (idx & 7));
}

bool te_inst_invert_bit(TaudInst *inst, int loopOffset, int loopLen)
{
    int len, idx;
    if (inst->invertMaskLen == 0) return false;
    len = imax(loopLen, 1);
    if (inst->invertMaskLen != ((len + 7) >> 3)) { inst->invertMaskLen = 0; return false; }
    idx = iclamp(loopOffset, 0, len - 1);
    return ((inst->invertMask[idx >> 3] >> (idx & 7)) & 1) != 0;
}

/* ── sample modification state ── */

void te_inst_clear_mod_state(TaudInst *inst)
{
    inst->modMaskLen = 0;
    inst->modRot = 0;
    inst->modSub = 0;
    inst->modScatter = 0;
    inst->modSeed = 0;
    inst->modOn = false;
    inst->modPrevRot = 0;
    inst->modPrevSub = 0;
    inst->modPrevScatter = 0;
    inst->modPrevSeed = 0;
    inst->modStepIndex = 0;
    inst->modXor = 0;
    inst->modPrevXor = 0;
    inst->modBitRot = 0;
    inst->modBitPermIdx = 0;
    inst->modBitPermOn = false;
    inst->modExtSwapA = -1;
    inst->modExtSwapB = -1;
    inst->modExtMirror = false;
    inst->modFunkWalk = -1;
    inst->modFunkPos = -1;
    inst->modFunkLen = 0;
}

bool te_inst_set_mod_region(TaudInst *inst, double from, double to)
{
    if (inst->modFrom == from && inst->modTo == to && inst->modCombBits == -1) return false;
    inst->modFrom = from;
    inst->modTo = to;
    inst->modCombBits = -1;
    inst->modCombOdd = false;
    inst->modEpoch++;
    te_inst_clear_mod_state(inst);
    return true;
}

bool te_inst_set_mod_comb(TaudInst *inst, int bits, bool odd)
{
    if (inst->modCombBits == bits && inst->modCombOdd == odd) return false;
    inst->modCombBits = bits;
    inst->modCombOdd = odd;
    inst->modEpoch++;
    te_inst_clear_mod_state(inst);
    return true;
}

bool te_inst_set_mod_op(TaudInst *inst, int op, bool invert)
{
    if (inst->modOp == op && inst->modInvert == invert && inst->modOpExt == 0) return false;
    inst->modOp = op;
    inst->modInvert = invert;
    inst->modOpExt = 0;
    inst->modF = 0;
    inst->modEpoch++;
    te_inst_clear_mod_state(inst);
    return true;
}

bool te_inst_set_mod_op_ext(TaudInst *inst, int code, bool invert, int f)
{
    if (inst->modOpExt == code && inst->modInvert == invert && inst->modF == f && inst->modOp == 0) return false;
    inst->modOp = 0;
    inst->modOpExt = code;
    inst->modInvert = invert;
    inst->modF = f;
    inst->modEpoch++;
    te_inst_clear_mod_state(inst);
    return true;
}

void te_inst_snapshot_mod_state(TaudInst *inst)
{
    inst->modPrevRot = inst->modRot;
    inst->modPrevSub = inst->modSub;
    inst->modPrevScatter = inst->modScatter;
    inst->modPrevSeed = inst->modSeed;
    inst->modPrevXor = inst->modXor;
}

void te_inst_reset_mod(TaudInst *inst)
{
    inst->modOp = 0;
    inst->modOpExt = 0;
    inst->modF = 0;
    inst->modInvert = false;
    inst->modFrom = 0;
    inst->modTo = 1;
    inst->modCombBits = -1;
    inst->modCombOdd = false;
    inst->modEpoch++;
    te_inst_clear_mod_state(inst);
}

void te_inst_toggle_mod_bit(TaudInst *inst, int i, int sampleLen)
{
    int len = imax(sampleLen, 1);
    int expectedSize = (len + 7) >> 3;
    int idx;
    if (expectedSize > MOD_MASK_CAP) expectedSize = MOD_MASK_CAP;
    if (inst->modMaskLen == 0 || inst->modMaskLen != expectedSize) {
        if (ensure_mask(&inst->modMask) == NULL) return;
        memset(inst->modMask, 0, (size_t)expectedSize);
        inst->modMaskLen = expectedSize;
    }
    idx = iclamp(i, 0, len - 1);
    if ((idx >> 3) < inst->modMaskLen) inst->modMask[idx >> 3] ^= (uint8_t)(1 << (idx & 7));
    inst->modOn = true;
}
