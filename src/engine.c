/* TaudEngine — the device facade (engine.js) plus the Playhead/TrackerState
 * parts of state.js that a player drives, and the rng.js seams. */

#include <stdlib.h>
#include <string.h>

#include "taud_internal.h"

/* ── randomness (rng.js) ── */

static double mulberry32(void *user)
{
    uint32_t *a = (uint32_t *)user, t;
    *a += 0x6d2b79f5u;
    t = *a;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

double te_random(TaudEngine *e)
{
    if (e->random != NULL) return e->random(e->randomUser);
    return mulberry32(&e->mulberry);
}

/* ── voices ── */

static const TaudEnvPoint DEFAULT_ENV_3F[ENV_POINTS] = {
    {0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},
    {0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},
    {0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0},{0x3f,0}
};
static const TaudEnvPoint DEFAULT_ENV_80[ENV_POINTS] = {
    {0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},
    {0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0},
    {0x80,0},{0x80,0},{0x80,0},{0x80,0},{0x80,0}
};

/* A fresh Voice, exactly as `new Voice()` leaves one (voice.js). */
void te_voice_init(TaudVoice *v)
{
    bool pooled = v->pooled;
    TaudVoice *nextFree = v->nextFree;
    memset(v, 0, sizeof *v);
    v->pooled = pooled;
    v->nextFree = nextFree;

    v->playbackRate = 1.0;
    v->currentPlaybackRate = 1.0;
    v->snapPlaybackRate = true;
    v->forward = true;
    v->sourceChannel = -1;
    v->activePatchIndex = -1;
    v->layerFixedNote = -1;
    v->layerMixGain = 1.0;
    v->nnaOverride = -1;
    v->volEnvOn = v->panEnvOn = v->pitchEnvOn = v->filterEnvOn = true;
    v->noteVolume = 0x3f;
    v->channelVolume = 0x3f;
    v->rowVolume = 63;
    v->channelPan = 0x80;
    v->rowPan = 32;
    v->panAzimuth = 128.0;
    v->spatialTargetAz = 128.0;
    v->currentMixVolume = 1.0;
    v->currentPan = 128.0;
    v->snapPan = true;
    v->envVolume = 1.0;
    v->envVolMix = 1.0;
    v->envPan = 0.5;
    v->envPitchValue = 0.5;
    v->envFilterValue = 0.5;
    v->fadeoutVolume = 1.0;
    v->activeChanCount = 1;
    v->right.nesDpcmCounter = 63;
    v->activeVolEnv = DEFAULT_ENV_3F;
    v->activePanEnv = DEFAULT_ENV_80;
    v->activePitchEnv = DEFAULT_ENV_80;
    v->activeFilterEnv = DEFAULT_ENV_80;
    v->activeDefaultCutoff = 0xff;
    v->activeDefaultResonance = 0xff;
    v->activeAttenGain = 1.0;
    v->nesDpcmCounter = 63;
    v->currentCutoff = 0xff;
    v->currentResonance = 0xff;
    v->filterA0 = 1.0;
    v->filterCutoffCached = -1;
    v->filterResonanceCached = -1;
    v->basePitch = 0x4000;
    v->amigaPeriod = -1.0;
    v->linearFreq = -1.0;
    v->tonePortaTarget = -1;
    v->tremorOnTime = 1;
    v->tremorOffTime = 1;
    v->tremorPhaseOn = true;
    v->vibratoRetrig = true;
    v->vibratoFineShift = 6;
    v->tremoloRetrig = true;
    v->panbrelloRetrig = true;
    v->noteDelayTick = -1;
    v->delayedVol = -1;
    v->noteActionTick = -1;
    v->delayedAction = -1;
    v->cutAtTick = -1;
    v->funkWalk = -1;
    v->funkPos = -1;
    v->funkWindow = -1;
    v->funkXfadeLen = 1;
    v->modGeom.epoch = -1;
    v->modGeom.base = -1;
    v->modGeom.len = -1;
    v->modFunkWindow = -1;
    v->modFunkXfadeLen = 1;
    v->panLawPan = NAN;
}

/* Background voices come from a pool. A voice leaves the background list
 * long before nothing refers to it any more — an FM rack keeps reading an
 * operator the list has dropped — so slots are reclaimed by marking what the
 * lanes and the list can still reach, exactly the objects the JS keeps alive. */

#define POOL_CHUNK 64

static void mark_voice(TaudVoice *v)
{
    /* Reachability is at most three hops (voice → rig → operator → parent),
     * so plain recursion is bounded. */
    int k;
    if (v == NULL || v->marked) return;
    v->marked = true;
    if (v->fmRig != NULL) for (k = 0; k < v->fmRig->count; k++) mark_voice(v->fmRig->voices[k]);
    mark_voice(v->fmParent);
}

static void pool_collect(TaudEngine *e)
{
    TaudVoiceChunk *c;
    int i;
    for (c = e->chunks; c != NULL; c = c->next) for (i = 0; i < c->count; i++) c->voices[i].marked = false;
    for (i = 0; i < MAX_VOICES; i++) e->voices[i].marked = false;
    for (i = 0; i < MAX_VOICES; i++) mark_voice(&e->voices[i]);
    for (i = 0; i < e->bgCount; i++) mark_voice(e->bg[i]);
    e->freeList = NULL;
    for (c = e->chunks; c != NULL; c = c->next) {
        for (i = 0; i < c->count; i++) {
            TaudVoice *v = &c->voices[i];
            if (!v->marked) { v->nextFree = e->freeList; e->freeList = v; }
        }
    }
}

static bool pool_grow(TaudEngine *e, int count)
{
    TaudVoiceChunk *c = (TaudVoiceChunk *)calloc(1, sizeof *c + sizeof(TaudVoice) * (size_t)(count - 1));
    int i;
    if (c == NULL) return false;
    c->count = count;
    c->next = e->chunks;
    e->chunks = c;
    for (i = 0; i < count; i++) {
        c->voices[i].pooled = true;
        c->voices[i].nextFree = e->freeList;
        e->freeList = &c->voices[i];
    }
    e->poolSize += count;
    return true;
}

TaudVoice *te_voice_alloc(TaudEngine *e)
{
    TaudVoice *v;
    if (e->freeList == NULL) {
        pool_collect(e);
        if (e->freeList == NULL && !pool_grow(e, POOL_CHUNK)) { e->oom = true; return NULL; }
    }
    v = e->freeList;
    e->freeList = v->nextFree;
    te_voice_init(v);
    return v;
}

void te_bg_push(TaudEngine *e, TaudVoice *v)
{
    if (e->bgCount == e->bgCap) {
        int nc = e->bgCap ? e->bgCap * 2 : 128;
        TaudVoice **nb = (TaudVoice **)realloc(e->bg, sizeof *nb * (size_t)nc);
        if (nb == NULL) { e->oom = true; return; }
        e->bg = nb;
        e->bgCap = nc;
    }
    e->bg[e->bgCount++] = v;
}

void te_bg_remove_at(TaudEngine *e, int i)
{
    memmove(&e->bg[i], &e->bg[i + 1], sizeof *e->bg * (size_t)(e->bgCount - i - 1));
    e->bgCount--;
}

/* ── patterns and cues ── */

const TaudCell *te_pattern_read(const TaudEngine *e, int idx)
{
    if (idx < 0 || idx >= NUM_PATTERNS || e->playdata[idx] == NULL) return e->emptyPattern;
    return e->playdata[idx];
}

static TaudCell *pattern_for(TaudEngine *e, int idx)
{
    TaudCell *p;
    int i;
    if (idx < 0 || idx >= NUM_PATTERNS) return NULL;
    p = e->playdata[idx];
    if (p == NULL) {
        p = (TaudCell *)calloc(ROWS_PER_PATTERN, sizeof *p);
        if (p == NULL) { e->oom = true; return NULL; }
        for (i = 0; i < ROWS_PER_PATTERN; i++) p[i].pan = 32;
        e->playdata[idx] = p;
    }
    return p;
}

static void cell_set_byte(TaudCell *c, int o, int b)
{
    switch (o) {
    case 0: c->note = (c->note & 0xff00) | b; break;
    case 1: c->note = (c->note & 0x00ff) | (b << 8); break;
    case 2: c->instrment = b; break;
    case 3: c->volume = b & 63; c->volumeEff = (b >> 6) & 3; break;
    case 4: c->pan = b & 63; c->panEff = (b >> 6) & 3; break;
    case 5: c->effect = b; break;
    case 6: c->effectArg = (c->effectArg & 0xff00) | b; break;
    case 7: c->effectArg = (c->effectArg & 0x00ff) | (b << 8); break;
    default: break;
    }
}

static void cell_set_byte_wide(TaudCell *c, int o, int b)
{
    switch (o) {
    case 0: c->note = (c->note & 0xff00) | b; break;
    case 1: c->note = (c->note & 0x00ff) | (b << 8); break;
    case 2: c->instrment = b; break;
    case 3: c->volume = b & 0xff; break;
    case 4: c->azimuth = (c->azimuth & 0x100) | b; break;
    case 5: c->effect = b; break;
    case 6: c->effectArg = (c->effectArg & 0xff00) | b; break;
    case 7: c->effectArg = (c->effectArg & 0x00ff) | (b << 8); break;
    case 8:
        c->azimuth = (c->azimuth & 0xff) | ((b & 0x80) << 1);
        c->volumeEff = (b >> 4) & 7;
        c->panEff = b & 0xf;
        break;
    case 9: c->elevation = (int8_t)(b >= 0x80 ? b - 0x100 : b); break;
    case 10: c->effect2 = b; break;
    case 11: c->effectArg2 = (c->effectArg2 & 0xff00) | b; break;
    case 12: c->effectArg2 = (c->effectArg2 & 0x00ff) | (b << 8); break;
    default: break;
    }
}

void te_upload_pattern(TaudEngine *e, int slot, const uint8_t *bytes, size_t len)
{
    TaudCell *pat = pattern_for(e, slot & 0x7fff);
    size_t i, n;
    if (pat == NULL) return;
    if (e->wideCells) {
        n = len < PATTERN_BYTES_WIDE ? len : PATTERN_BYTES_WIDE;
        for (i = 0; i < n; i++) cell_set_byte_wide(&pat[i / CELL_BYTES_WIDE], (int)(i % CELL_BYTES_WIDE), bytes[i]);
        return;
    }
    n = len < PATTERN_BYTES ? len : PATTERN_BYTES;
    for (i = 0; i < n; i++) cell_set_byte(&pat[i / CELL_BYTES], (int)(i % CELL_BYTES), bytes[i]);
}

static TaudPlayInst decode_inst_word(int w)
{
    TaudPlayInst r = { INST_NOP, 0, 0 };
    int b30, b31;
    if (w == 0) return r;
    b30 = (w >> 8) & 0xff;
    b31 = w & 0xff;
    if (b30 == 0x02) { r.type = INST_PATLEN; r.arg = r.rows = (b31 & 0x3f) + 1; return r; }
    if (b30 == 0x01) {
        if ((b31 & 0xc0) == 0x40) {
            int x = b31 & 0x3f;
            r.type = INST_HALTAT;
            r.arg = r.rows = x == 0 ? 64 : x;
            return r;
        }
        r.type = INST_HALT;
        return r;
    }
    if ((b30 & 0xf0) == 0x80) { r.type = INST_GOBACK; r.arg = ((b30 & 0xf) << 8) | b31; return r; }
    if ((b30 & 0xf0) == 0x90) { r.type = INST_SKIP; r.arg = ((b30 & 0xf) << 8) | b31; return r; }
    if ((b30 & 0xf0) == 0xf0) { r.type = INST_JUMP; r.arg = ((b30 & 0xf) << 8) | b31; return r; }
    return r;
}

static int cue_inst_word(const TaudCue *c, int base)
{
    int w = 0, k;
    for (k = 0; k < 16; k++) w |= ((c->raw[base + k] >> 15) & 1) << k;
    return w;
}

static void cue_init(TaudCue *c)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) c->raw[i] = PATTERN_EMPTY;
    c->inst0.type = c->inst1.type = INST_NOP;
    c->inst0.arg = c->inst1.arg = 0;
    c->inst0.rows = c->inst1.rows = 0;
}

const TaudCue *te_cue_at(const TaudEngine *e, int idx)
{
    const TaudCue *block;
    idx &= NUM_CUES - 1;
    block = e->cueBlocks[idx / CUE_BLOCK];
    return block != NULL ? &block[idx % CUE_BLOCK] : &e->emptyCue;
}

void te_upload_cue(TaudEngine *e, int idx, const uint8_t *bytes, size_t len)
{
    TaudCue *block, *cue;
    size_t n = e->is64ChannelMode ? 128 : 64, i;
    idx &= NUM_CUES - 1;
    block = e->cueBlocks[idx / CUE_BLOCK];
    if (block == NULL) {
        int k;
        block = (TaudCue *)malloc(sizeof(TaudCue) * CUE_BLOCK);
        if (block == NULL) { e->oom = true; return; }
        for (k = 0; k < CUE_BLOCK; k++) cue_init(&block[k]);
        e->cueBlocks[idx / CUE_BLOCK] = block;
    }
    cue = &block[idx % CUE_BLOCK];
    if (len < n) n = len;
    for (i = 0; i < n; i++) {
        int ch = (int)(i >> 1);
        cue->raw[ch] = (i & 1) == 0 ? (uint16_t)((cue->raw[ch] & 0xff00) | bytes[i])
                                    : (uint16_t)((cue->raw[ch] & 0x00ff) | (bytes[i] << 8));
    }
    cue->inst0 = decode_inst_word(cue_inst_word(cue, 0));
    cue->inst1 = decode_inst_word(cue_inst_word(cue, 16));
}

/* ── content upload ── */

/* Install a sample+instrument image: `pool` is borrowed (the engine never
 * writes a sample), `instBin` holds the 1024 256-byte instrument records. */
void te_upload_sample_inst_blob(TaudEngine *e, const uint8_t *pool, size_t poolLen, const uint8_t *instBin)
{
    int i;
    e->sampleBin = pool;
    e->samplePoolLen = poolLen;
    for (i = 0; i < 1024; i++) te_inst_load_record(&e->instruments[i], instBin + (size_t)i * 256);
    for (i = 0; i < NUM_INST_SLOTS; i++) {
        free(e->instruments[i].extraPatches);
        e->instruments[i].extraPatches = NULL;
        e->instruments[i].patchCount = 0;
    }
}

void te_upload_instrument_patches(TaudEngine *e, int slot, const uint8_t *bytes, size_t len)
{
    TaudInst *inst = &e->instruments[slot & 0x3ff];
    TaudPatch *patches = NULL;
    int n = te_parse_patches_blob(bytes, len, &patches);
    free(inst->extraPatches);
    inst->extraPatches = n > 0 ? patches : NULL;
    inst->patchCount = n > 0 ? n : 0;
    if (n < 0) e->oom = true;
}

void te_set_cell_format(TaudEngine *e, bool wide)
{
    int i;
    e->wideCells = wide;
    if (e->ts_wideCells == wide) return;
    e->ts_wideCells = wide;
    e->volMax = wide ? VOLUME_MAX_WIDE : VOLUME_MAX;
    e->volStep = wide ? VOLUME_STEP_WIDE : 1;
    e->volDiv = e->volMax * 1.0;
    for (i = 0; i < MAX_VOICES; i++) {
        TaudVoice *v = &e->voices[i];
        v->noteVolume = e->volMax;
        v->channelVolume = e->volMax;
        v->channelVolumeSet = false;
        v->rowVolume = e->volMax;
    }
}

void te_set_tuning(TaudEngine *e, int baseNote, double freq)
{
    e->tuningBaseNote = baseNote & 0xffff;
    e->tuningFreq = freq;
    e->tuningRatio = te_tuning_ratio_of(e->tuningBaseNote, e->tuningFreq);
}

void te_update_global_behaviour(TaudEngine *e, int flags)
{
    e->toneMode = flags & 3;
    e->interpolationMode = (flags >> 2) & 7;
}

void te_set_tracker_mixer_flags(TaudEngine *e, int flags)
{
    e->initialGlobalFlags = flags;
    te_update_global_behaviour(e, flags);
}

/* (Re-)install the surround model with the monitor's render target. */
static void apply_surround_model(TaudEngine *e)
{
    int model = e->surroundModel & 3;
    e->ts_surroundModel = model;
    free(e->spatial);
    e->spatial = NULL;
    e->rendererGen++;
    if (model == SURROUND_STEREO) return;
    e->spatial = (TaudSpatialBus *)calloc(1, sizeof *e->spatial);
    if (e->spatial == NULL) { e->oom = true; e->ts_surroundModel = SURROUND_STEREO; return; }
    if (e->monitorMode == MONITOR_BINAURAL) {
        bool sphere = model == SURROUND_SPATIAL;
        if (e->binaural == NULL || e->binaural->sphere != sphere) {
            te_binaural_free(e->binaural);
            e->binaural = te_binaural_new(sphere, e->rate);
        }
        if (e->binaural != NULL) {
            te_binaural_reset(e->binaural);
            e->spatial->kind = 1;
            e->spatial->bin = e->binaural;
            e->spatial->numChannels = e->binaural->numChannels;
            return;
        }
        e->oom = true;
    }
    e->spatial->kind = 0;
    e->spatial->numChannels = 2;
}

void te_set_surround_model(TaudEngine *e, int model)
{
    e->surroundModel = model & 3;
    apply_surround_model(e);
}

void te_set_monitor_mode(TaudEngine *e, int mode)
{
    e->monitorMode = mode & 1;
    apply_surround_model(e);
}

void te_set_mastering(TaudEngine *e, const TaudMastering *params)
{
    if (params != NULL) { e->masteringParams = *params; e->hasMasteringParams = true; }
    else e->hasMasteringParams = false;
    if (!te_mastering_engaged(params)) {
        te_master_chain_free(e->mastering);
        e->mastering = NULL;
        return;
    }
    if (e->mastering == NULL) {
        e->mastering = te_master_chain_new(params, e->rate);
        if (e->mastering == NULL) e->oom = true;
    } else {
        te_master_chain_set_params(e->mastering, params);
    }
}

/* ── transport ── */

void te_set_cue_position(TaudEngine *e, int pos)
{
    e->position = pos & (NUM_CUES - 1);
    e->cuePos = e->position;
}

void te_set_tracker_row(TaudEngine *e, int row)
{
    int i;
    e->rowIndex = iclamp(row, 0, 63);
    e->tickInRow = 0;
    e->samplesIntoTick = 0.0;
    e->firstRow = true;
    e->pendingOrderJump = -1;
    e->pendingRowJump = -1;
    e->pendingRowJumpLocal = false;
    e->patternDelayRemaining = 0;
    e->patternDelayActive = false;
    e->sexWinningChannel = -1;
    e->finePatternDelayExtra = 0;
    e->pendingInterrupts = 0;
    memset(e->interruptArgs, 0, sizeof e->interruptArgs);
    for (i = 0; i < MAX_VOICES; i++) {
        TaudVoice *v = &e->voices[i];
        v->active = false;
        v->loopStartRow = 0; v->loopCount = 0;
        v->dittoActive = false; v->dittoSourceStart = 0; v->dittoLength = 0; v->dittoEndRow = 0;
        v->channelVolume = e->volMax;
        v->channelPan = 0x80;
        v->channelVolumeSet = false;
        v->channelPanSet = false;
        v->rowPan = 32;
        v->panAzimuth = 128.0;
        v->panElevation = 0.0;
        v->notePan = 0;
        v->noteElevation = 0.0;
        v->spatialTargetAz = 128.0;
        v->spatialTargetEl = 0.0;
        v->spatialSlideActive = false;
        v->panbrelloOffset = 0;
        v->glissandoOn = false;
        v->clipMode = 0;
        v->bitcrusherDepth = 0;
        v->bitcrusherSkip = 0;
        v->bitcrusherCounter = 0;
        v->bitcrusherHeld = 0.0;
        v->overdriveAmp = 0;
        v->right.bitcrusherCounter = 0;
        v->right.bitcrusherHeld = 0.0;
        v->nnaOverride = -1;
        v->volEnvOn = true; v->panEnvOn = true; v->pitchEnvOn = true; v->filterEnvOn = true;
    }
    e->bgCount = 0;
    if (e->mastering != NULL) te_master_chain_reset(e->mastering);
    te_reconstruct_ditto_state(e, e->rowIndex);
}

void te_play(TaudEngine *e) { e->isPlaying = true; }

void te_silence_song_voices(TaudEngine *e, bool ramp)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) {
        TaudVoice *v = &e->voices[i];
        if (!v->active) continue;
        if (ramp) te_start_cut_ramp(e, v); else v->active = false;
    }
    for (i = 0; i < e->bgCount; i++) {
        TaudVoice *bg = e->bg[i];
        if (!bg->active) continue;
        if (ramp) te_start_cut_ramp(e, bg); else bg->active = false;
    }
}

void te_stop(TaudEngine *e)
{
    e->isPlaying = false;
    te_silence_song_voices(e, false);
}

void te_reset_sample_fx_state(TaudEngine *e)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) {
        TaudVoice *v = &e->voices[i];
        v->invertSpeed = 0;
        v->invertAccumulator = 0;
        v->invertWritePos = 0;
        v->funkSpeed = 0;
        v->funkMode = 0;
        v->funkAccumulator = 0;
        v->funkWalk = -1;
        v->funkPos = -1;
        v->funkWindow = -1;
        v->funkXfade = 0;
        v->modFunkWindow = -1;
        v->modFunkXfade = 0;
        v->modPeriod = 0;
        v->modTickCount = 0;
        v->modWritePos = 0;
        v->modXfade = 0;
    }
    for (i = 0; i < NUM_INST_SLOTS; i++) {
        e->instruments[i].invertMaskLen = 0;
        te_inst_reset_mod(&e->instruments[i]);
    }
}

unsigned te_drain_interrupts(TaudEngine *e)
{
    unsigned m = e->pendingInterrupts;
    e->pendingInterrupts = 0;
    return m;
}

bool te_render_chunk(TaudEngine *e) { return te_generate_tracker_audio(e); }

/* ── lifetime ── */

TaudEngine *te_engine_new(int rate)
{
    TaudEngine *e = (TaudEngine *)calloc(1, sizeof *e);
    int i;
    if (e == NULL) return NULL;
    e->rate = rate;
    e->rampOutSamples = (int)js_round(RAMP_OUT_SEC * rate);
    e->volRampSamples = (int)js_round(VOL_RAMP_SEC * rate);
    e->attackRampSamples = (int)js_round(ATTACK_RAMP_SEC * rate);
    if (e->volRampSamples < 1) e->volRampSamples = 1;
    if (e->attackRampSamples < 1) e->attackRampSamples = 1;
    if (e->rampOutSamples < 1) e->rampOutSamples = 1;
    te_build_tables(e);

    e->instruments = (TaudInst *)calloc(NUM_INST_SLOTS, sizeof(TaudInst));
    e->playdata = (TaudCell **)calloc(NUM_PATTERNS, sizeof(TaudCell *));
    if (e->instruments == NULL || e->playdata == NULL) {
        te_engine_free(e);
        return NULL;
    }
    for (i = 0; i < NUM_INST_SLOTS; i++) te_inst_init(&e->instruments[i], i);
    for (i = 0; i < ROWS_PER_PATTERN; i++) e->emptyPattern[i].pan = 32;
    cue_init(&e->emptyCue);

    /* Playhead defaults. */
    e->masterPan = 128;
    e->bpm = 125;
    e->tickRate = 6;
    e->globalVolume = 0x80;
    e->mixingVolume = 0x80;

    /* TrackerState defaults. */
    e->firstRow = true;
    for (i = 0; i < MAX_VOICES; i++) te_voice_init(&e->voices[i]);
    e->volMax = VOLUME_MAX;
    e->volStep = 1;
    e->volDiv = 63.0;
    e->tuningRatio = 1.0;
    te_mastering_default(&e->masteringParams);
    e->hasMasteringParams = true;
    e->pendingOrderJump = -1;
    e->pendingRowJump = -1;
    e->sexWinningChannel = -1;

    e->xorshift = 0x9e3779b9u;
    e->mulberry = 1;
    if (!pool_grow(e, POOL_CHUNK * 2)) { te_engine_free(e); return NULL; }
    return e;
}

void te_engine_free(TaudEngine *e)
{
    int i;
    TaudVoiceChunk *c;
    if (e == NULL) return;
    if (e->instruments != NULL) {
        for (i = 0; i < NUM_INST_SLOTS; i++) te_inst_free(&e->instruments[i]);
        free(e->instruments);
    }
    if (e->playdata != NULL) {
        for (i = 0; i < NUM_PATTERNS; i++) free(e->playdata[i]);
        free(e->playdata);
    }
    for (i = 0; i < CUE_BLOCKS; i++) free(e->cueBlocks[i]);
    free(e->bg);
    c = e->chunks;
    while (c != NULL) { TaudVoiceChunk *n = c->next; free(c); c = n; }
    free(e->spatial);
    te_binaural_free(e->binaural);
    te_master_chain_free(e->mastering);
    free(e);
}
