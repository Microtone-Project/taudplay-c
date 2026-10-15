/* Trigger path + NNA/Metainstrument machinery — port of trigger.js. */

#include <string.h>

#include "taud_internal.h"

/* What the INSTRUMENT left on the note-pan axis for the trigger just run:
 * [notePan, noteElevation, present]. Read only by triggerMetaOrNote. */
typedef struct { double pan, elevation; bool present; } NotePanSeed;

static void trigger_note_seed(TaudEngine *e, TaudVoice *v, int noteVal, int instId, int volOverride, NotePanSeed *seed);

void te_apply_active_sample(TaudVoice *v, const TaudInst *inst, const TaudPatch *patch);

static void resolve_active_envelopes(TaudVoice *v, const TaudInst *inst, const TaudPatch *patch)
{
    const TaudEnvPoint *pitEnv = inst->pfEnvelopes, *filEnv = inst->pfEnvelopes;
    int pitLoop = 0, pitSus = 0, filLoop = 0, filSus = 0;
    bool pitOn = false, filOn = false;

    if (patch != NULL && patch->hasVolEnv) {
        v->activeVolEnv = patch->volEnv;
        v->activeVolEnvLoop = patch->volEnvLoop;
        v->activeVolEnvSustain = patch->volEnvSustain;
    } else {
        v->activeVolEnv = inst->volEnvelopes;
        v->activeVolEnvLoop = inst->volEnvLoop;
        v->activeVolEnvSustain = inst->volEnvSustainWord;
    }
    /* The volume envelope answers to its P bit like every other envelope (spec
     * §7.2): a record that says "absent" plays at unity whatever its nodes hold. */
    v->hasVolEnv = te_env_present(v->activeVolEnvLoop);
    if (patch != NULL && patch->hasPanEnv) {
        v->activePanEnv = patch->panEnv;
        v->activePanEnvLoop = patch->panEnvLoop;
        v->activePanEnvSustain = patch->panEnvSustain;
    } else {
        v->activePanEnv = inst->panEnvelopes;
        v->activePanEnvLoop = inst->panEnvLoop;
        v->activePanEnvSustain = inst->panEnvSustainWord;
    }

    if (te_env_present(inst->pfEnvLoop)) {
        if (((inst->pfEnvLoop >> 7) & 1) != 0) {
            filEnv = inst->pfEnvelopes; filLoop = inst->pfEnvLoop; filSus = inst->pfEnvSustainWord; filOn = true;
        } else {
            pitEnv = inst->pfEnvelopes; pitLoop = inst->pfEnvLoop; pitSus = inst->pfEnvSustainWord; pitOn = true;
        }
    }
    if (te_env_present(inst->pf2EnvLoop)) {
        if (((inst->pf2EnvLoop >> 7) & 1) != 0) {
            filEnv = inst->pf2Envelopes; filLoop = inst->pf2EnvLoop; filSus = inst->pf2EnvSustainWord; filOn = true;
        } else {
            pitEnv = inst->pf2Envelopes; pitLoop = inst->pf2EnvLoop; pitSus = inst->pf2EnvSustainWord; pitOn = true;
        }
    }
    if (patch != NULL && patch->hasPitchEnv) {
        pitEnv = patch->pitchEnv; pitLoop = patch->pitchEnvLoop; pitSus = patch->pitchEnvSustain;
        pitOn = te_env_present(patch->pitchEnvLoop);
    }
    if (patch != NULL && patch->hasFilterEnv) {
        filEnv = patch->filterEnv; filLoop = patch->filterEnvLoop; filSus = patch->filterEnvSustain;
        filOn = te_env_present(patch->filterEnvLoop);
    }
    v->activePitchEnv = pitEnv; v->activePitchEnvLoop = pitLoop;
    v->activePitchEnvSustain = pitSus; v->hasPitchEnv = pitOn;
    v->activeFilterEnv = filEnv; v->activeFilterEnvLoop = filLoop;
    v->activeFilterEnvSustain = filSus; v->hasFilterEnv = filOn;

    if (patch != NULL && patch->hasExtra) {
        v->activeFadeoutStep = patch->fadeoutStep;
        v->filterSfMode = patch->filterSfMode;
        v->activeDefaultCutoff = patch->extraCutoff;
        v->activeDefaultResonance = patch->extraResonance;
        v->activeAttenGain = te_atten_gain_of(patch->extraInitialAttenOctet);
    } else {
        v->activeFadeoutStep = inst->volumeFadeoutLow | ((inst->fadeoutHigh & 0x0f) << 8);
        v->filterSfMode = inst_filter_sf_mode(inst);
        v->activeDefaultCutoff = inst_default_cutoff16(inst);
        v->activeDefaultResonance = inst_default_resonance16(inst);
        v->activeAttenGain = te_atten_gain_of(inst->initialAttenOctet);
    }
}

void te_apply_active_sample(TaudVoice *v, const TaudInst *inst, const TaudPatch *patch)
{
    v->activePatchIndex = patch == NULL || inst->extraPatches == NULL ? -1 : (int)(patch - inst->extraPatches);
    if (patch == NULL) {
        v->activeSamplePtr = inst->samplePtr;
        v->activeSampleLength = inst->sampleLength;
        v->activeSamplePlayStart = inst->samplePlayStart;
        v->activeSampleLoopStart = inst->sampleLoopStart;
        v->activeSampleLoopEnd = inst->sampleLoopEnd;
        /* An OPL instrument's rate is its C2SPD; 0 plays at the tracker
         * convention (§7.7) — the pitch has to come from somewhere. */
        v->activeSamplingRate = inst->opl != NULL && inst->samplingRate == 0 ? OPL_C4_RATE : inst->samplingRate;
        v->activeSampleDetune = inst_sample_detune_signed(inst);
        v->activeLoopMode = inst->loopMode;
        v->activeVibratoSpeed = inst->vibratoSpeed;
        v->activeVibratoSweep = inst->vibratoSweep;
        v->activeVibratoDepth = inst->vibratoDepth;
        v->activeVibratoRate = inst->vibratoRate;
        v->activeVibratoWaveform = inst_vibrato_waveform(inst);
        v->activeChanCount = 1;
        v->activeChanMode = 0;
        v->activeChanPtr2 = 0;
    } else {
        bool vibDefer = te_patch_vibrato_inherits(patch);
        v->activeSamplePtr = patch->samplePtr;
        v->activeSampleLength = patch->sampleLength;
        v->activeSamplePlayStart = patch->playStart;
        v->activeSampleLoopStart = patch->loopStart;
        v->activeSampleLoopEnd = patch->loopEnd;
        v->activeSamplingRate = patch->samplingRate;
        v->activeSampleDetune = patch->sampleDetune;
        v->activeLoopMode = patch->loopMode;
        v->activeVibratoSpeed = vibDefer ? inst->vibratoSpeed : patch->vibratoSpeed;
        v->activeVibratoSweep = vibDefer ? inst->vibratoSweep : patch->vibratoSweep;
        v->activeVibratoDepth = vibDefer ? inst->vibratoDepth : patch->vibratoDepth;
        v->activeVibratoRate = vibDefer ? inst->vibratoRate : patch->vibratoRate;
        v->activeVibratoWaveform = patch->vibratoWaveform == 0xff ? inst_vibrato_waveform(inst) : patch->vibratoWaveform;
        if (te_patch_is_stereo(patch)) {
            v->activeChanCount = 2;
            v->activeChanMode = patch->chanMode;
            v->activeChanPtr2 = patch->chanPtrs[0];
        } else {
            v->activeChanCount = 1;
            v->activeChanMode = 0;
            v->activeChanPtr2 = 0;
        }
    }
    resolve_active_envelopes(v, inst, patch);
}

int te_row_volume_from_default(const TaudInst *inst, const TaudPatch *patch, int volMax)
{
    int dnv = patch != NULL && patch->defaultNoteVolume != 0 ? patch->defaultNoteVolume : inst->defaultNoteVolume;
    if (dnv == 0) return volMax;
    return volMax == 0xff ? dnv : (dnv * 63 + 127) / 255;
}

void te_cap_background_voices(TaudEngine *e)
{
    while (e->bgCount > MAX_BG_VOICES) {
        int idx = -1, i;
        TaudVoice *culled;
        for (i = 0; i < e->bgCount; i++) if (!e->bg[i]->isLayerChild && !e->bg[i]->fmOperator) { idx = i; break; }
        if (idx < 0) for (i = 0; i < e->bgCount; i++) if (!e->bg[i]->fmOperator) { idx = i; break; }
        if (idx < 0) idx = 0;
        culled = e->bg[idx];
        culled->active = false;
        te_bg_remove_at(e, idx);
        if (culled->fmRig != NULL) {
            for (i = e->bgCount - 1; i >= 0; i--) {
                TaudVoice *bg = e->bg[i];
                if (!bg->fmOperator || bg->fmParent != culled) continue;
                bg->active = false;
                bg->fmOperator = false;
                bg->fmParent = NULL;
                bg->isLayerChild = false;
                te_bg_remove_at(e, i);
            }
        }
    }
}

static void release_layer_children(TaudEngine *e, int vi)
{
    int override = e->voices[vi].nnaOverride, i;
    for (i = 0; i < e->bgCount; i++) {
        TaudVoice *bg = e->bg[i];
        int nna;
        if (!bg->isLayerChild || bg->sourceChannel != vi) continue;
        if (bg->fmOperator) continue;
        bg->isLayerChild = false;
        bg->layerPitchMod = 0;
        nna = override >= 0 ? override : inst_new_note_action(&e->instruments[bg->instrumentId]);
        switch (nna) {
        case 0:
        case 4:
            if (!bg->keyOff) { bg->keyOff = true; te_apply_key_lift(bg, &e->instruments[bg->instrumentId]); }
            break;
        case 1: bg->active = false; break;
        case 3: bg->noteFading = true; break;
        default: break;
        }
    }
}

void te_cut_layer_children(TaudEngine *e, int vi)
{
    int i;
    for (i = 0; i < e->bgCount; i++) {
        if (te_is_sounding_child(e, e->bg[i], vi)) te_start_cut_ramp(e, e->bg[i]);
    }
}

static inline int pitch_of(const TaudMetaLayer *l, int noteVal)
{
    return iclamp(te_layer_note(l, noteVal), 0x20, 0xffff);
}

static void seed_child_lane_context(TaudVoice *child, const TaudVoice *voice, int chanPan, int chanRowPan,
                                    int chanPanbrello, double chanAzimuth, double chanElevation)
{
    child->channelVolume = voice->channelVolume;
    child->channelVolumeSet = voice->channelVolumeSet;
    child->channelPanSet = voice->channelPanSet;
    child->channelPan = chanPan;
    child->rowPan = chanRowPan;
    child->panbrelloOffset = chanPanbrello;
    child->panAzimuth = chanAzimuth;
    child->panElevation = chanElevation;
    child->clipMode = voice->clipMode;
    child->bitcrusherDepth = voice->bitcrusherDepth;
    child->bitcrusherSkip = voice->bitcrusherSkip;
    child->overdriveAmp = voice->overdriveAmp;
}

static void trigger_fm_rack(TaudEngine *e, TaudVoice *voice, int vi, int noteVal, const TaudInst *inst,
                            int rowVolOverride, int seedVol);

static bool fm_sounds(TaudEngine *e, const TaudMetaLayer *o, int noteVal, int seedVol)
{
    /* An OPL instrument is a channel, not a waveform a rack can read (§7.7);
     * a formant instrument is a sound made of a waveform, not one (§7.8). */
    return o->instIdx >= 1 && o->instIdx <= 1023 && !inst_is_meta(&e->instruments[o->instIdx]) &&
        !inst_is_opl(&e->instruments[o->instIdx]) && !inst_is_formant(&e->instruments[o->instIdx]) &&
        noteVal >= o->pitchStart && noteVal <= o->pitchEnd &&
        seedVol >= o->volStart && seedVol <= o->volEnd;
}

void te_trigger_meta_or_note(TaudEngine *e, TaudVoice *voice, int vi, int noteVal, int instId, int rowVolOverride)
{
    const TaudInst *inst;
    const TaudMetaLayer *layers[META_MAX_LAYERS];
    const TaudMetaLayer *l0;
    int nLayers, gateVol, seedVol, k;
    int chanPan, chanRowPan, chanPanbrello;
    double chanAzimuth, chanElevation, l0Pan, l0Elevation;
    bool l0HasPan;
    NotePanSeed seed;

    if (instId != 0) voice->displayInst = instId;
    release_layer_children(e, vi);
    te_drop_fm_operators(e, vi);
    inst = instId != 0 ? &e->instruments[instId] : &e->instruments[voice->instrumentId];
    if (!inst_is_meta(inst)) {
        te_trigger_note(e, voice, noteVal, instId, rowVolOverride);
        voice->layerMixGain = 1.0;
        voice->layerRelDetune = 0;
        voice->layerFixedNote = -1;
        voice->layerRelPan = 0;
        voice->layerRelElevation = 0;
        voice->isLayerChild = false;
        return;
    }
    gateVol = e->ts_wideCells ? asr32(rowVolOverride, 2) : rowVolOverride;
    seedVol = gateVol >= 0 && gateVol <= 0x3f ? gateVol : 0x3f;
    if (inst_is_fm(inst)) {
        trigger_fm_rack(e, voice, vi, noteVal, inst, rowVolOverride, seedVol);
        return;
    }
    nLayers = te_inst_resolve_meta_layers(inst, noteVal, seedVol, layers);
    if (inst->meta->strict) {
        int n = 0;
        for (k = 0; k < nLayers; k++) {
            const TaudMetaLayer *l = layers[k];
            if (te_inst_resolve_patch(&e->instruments[l->instIdx], pitch_of(l, noteVal), seedVol) != NULL) layers[n++] = l;
        }
        nLayers = n;
    }
    if (nLayers == 0) {
        voice->active = false;
        voice->layerMixGain = 1.0;
        voice->layerRelDetune = 0;
        voice->layerFixedNote = -1;
        return;
    }
    l0 = layers[0];
    chanPan = voice->channelPan; chanRowPan = voice->rowPan;
    chanPanbrello = voice->panbrelloOffset;
    chanAzimuth = voice->panAzimuth; chanElevation = voice->panElevation;
    trigger_note_seed(e, voice, pitch_of(l0, noteVal), l0->instIdx, rowVolOverride, &seed);
    l0HasPan = seed.present;
    l0Pan = l0HasPan ? seed.pan : 0;
    l0Elevation = l0HasPan ? seed.elevation : 0;
    voice->layerMixGain = META_MIX_GAIN[l0->mixOctet & 0xff];
    voice->layerRelDetune = 0;
    voice->layerFixedNote = l0->fixedPitch ? pitch_of(l0, noteVal) : -1;
    voice->layerRelPan = 0;
    voice->layerRelElevation = 0;
    voice->isLayerChild = false;
    voice->metaForeground = true;
    voice->metaForegroundDetune = l0->detune;
    for (k = 1; k < nLayers; k++) {
        const TaudMetaLayer *lk = layers[k];
        TaudVoice *child = te_voice_alloc(e);
        if (child == NULL) break;
        seed_child_lane_context(child, voice, chanPan, chanRowPan, chanPanbrello, chanAzimuth, chanElevation);
        trigger_note_seed(e, child, pitch_of(lk, noteVal), lk->instIdx, rowVolOverride, &seed);
        child->isLayerChild = true;
        child->sourceChannel = vi;
        child->displayInst = voice->displayInst;
        child->layerRelDetune = te_layer_note(lk, noteVal) - te_layer_note(l0, noteVal);
        child->layerFixedNote = lk->fixedPitch ? pitch_of(lk, noteVal) : -1;
        if (seed.present) {
            child->layerRelPan = seed.pan - l0Pan;
            child->layerRelElevation = seed.elevation - l0Elevation;
        } else {
            child->layerRelPan = 0;
            child->layerRelElevation = 0;
        }
        child->notePan = te_bound_note_pan(e, voice->notePan + child->layerRelPan);
        child->noteElevation = voice->noteElevation + child->layerRelElevation;
        child->layerMixGain = META_MIX_GAIN[lk->mixOctet & 0xff];
        te_bg_push(e, child);
    }
    te_cap_background_voices(e);
}

static void trigger_fm_rack(TaudEngine *e, TaudVoice *voice, int vi, int noteVal, const TaudInst *inst,
                            int rowVolOverride, int seedVol)
{
    const TaudMeta *meta = inst->meta;
    const TaudMetaLayer *ops = meta->layers;
    uint8_t referenced[FM_MAX_OPERATORS];
    TaudFmRig *rig;
    int chanPan, chanRowPan, chanPanbrello, k;
    double chanAzimuth, chanElevation;

    if (!meta->hasProgram || !fm_sounds(e, &ops[0], noteVal, seedVol)) {
        voice->active = false;
        voice->fmRig = NULL;
        voice->layerMixGain = 1.0;
        voice->layerRelDetune = 0;
        voice->layerFixedNote = -1;
        return;
    }
    te_fm_referenced_operators(meta->program, meta->programLen, meta->count, referenced);

    chanPan = voice->channelPan; chanRowPan = voice->rowPan;
    chanPanbrello = voice->panbrelloOffset;
    chanAzimuth = voice->panAzimuth; chanElevation = voice->panElevation;
    te_trigger_note(e, voice, iclamp(noteVal + ops[0].detune, 0x20, 0xffff), ops[0].instIdx, rowVolOverride);
    voice->layerMixGain = 1.0;
    voice->layerRelDetune = 0;
    voice->layerFixedNote = -1;
    voice->layerRelPan = 0;
    voice->layerRelElevation = 0;
    voice->isLayerChild = false;
    voice->metaForeground = true;
    voice->metaForegroundDetune = ops[0].detune;

    /* The rig is built after the trigger (which drops any previous one); the
     * JS builds it before, but nothing in between reads it. */
    rig = &voice->rigStore;
    te_fm_rig_init(rig, meta->count);
    rig->program = meta->program;
    rig->programLen = meta->programLen;
    te_fm_seed_gains(rig, meta);
    voice->fmRig = rig;
    rig->voices[0] = voice;

    for (k = 1; k < meta->count; k++) {
        TaudVoice *op;
        if (referenced[k] == 0 || !fm_sounds(e, &ops[k], noteVal, seedVol)) continue;
        op = te_voice_alloc(e);
        if (op == NULL) break;
        seed_child_lane_context(op, voice, chanPan, chanRowPan, chanPanbrello, chanAzimuth, chanElevation);
        te_trigger_note(e, op, iclamp(noteVal + ops[k].detune, 0x20, 0xffff), ops[k].instIdx, rowVolOverride);
        op->isLayerChild = true;
        op->fmOperator = true;
        op->fmParent = voice;
        op->sourceChannel = vi;
        op->displayInst = voice->displayInst;
        op->layerRelDetune = ops[k].detune - ops[0].detune;
        op->layerMixGain = 1.0;
        op->layerRelPan = 0;
        op->layerRelElevation = 0;
        op->notePan = voice->notePan;
        op->noteElevation = voice->noteElevation;
        rig->voices[k] = op;
        te_bg_push(e, op);
    }
    te_cap_background_voices(e);
}

int te_narrow_vol_axis(const TaudEngine *e, int v)
{
    return iclamp(e->ts_wideCells ? asr32(v, 2) : v, 0, 0x3f);
}

void te_trigger_note(TaudEngine *e, TaudVoice *voice, int noteVal, int instId, int volOverride)
{
    NotePanSeed seed;
    trigger_note_seed(e, voice, noteVal, instId, volOverride, &seed);
}

static void trigger_note_seed(TaudEngine *e, TaudVoice *voice, int noteVal, int instId, int volOverride, NotePanSeed *seed)
{
    bool mayCarry = voice->active && !voice->keyOff && !voice->noteFading &&
        (instId == 0 || instId == voice->instrumentId);
    /* An OPL channel handed over by te_maybe_spawn_background_for_nna (a
     * retrigger the New Note Action cuts) keeps its envelopes, as the chip
     * does — read before anything below marks the voice active. */
    bool oplCarry = voice->oplCarry && voice->active && voice->opl != NULL;
    const TaudInst *inst;
    const TaudPatch *patch;
    int seedVolForLookup;

    voice->oplCarry = false;

    if (instId != 0) voice->instrumentId = instId;
    inst = &e->instruments[voice->instrumentId];
    if (volOverride >= 0) seedVolForLookup = te_narrow_vol_axis(e, volOverride);
    else if (instId != 0) seedVolForLookup = te_row_volume_from_default(inst, NULL, 0x3f);
    else seedVolForLookup = te_narrow_vol_axis(e, voice->noteVolume);
    patch = te_inst_resolve_patch(inst, noteVal, seedVolForLookup);
    te_apply_active_sample(voice, inst, patch);
    voice->tonePortaTarget = -1;
    voice->samplePos = voice->activeSamplePlayStart;
    voice->forward = true;
    voice->active = true;
    voice->keyOff = false;
    if (!(mayCarry && te_env_carry(voice->activeVolEnvLoop))) {
        voice->envIndex = 0;
        voice->envTimeSec = 0.0;
        voice->envVolume = dclamp(voice->activeVolEnv[0].value / 63.0, 0.0, 1.0);
        voice->envVolMix = voice->envVolume;
        voice->envVolStep = 0.0;
    }
    voice->hasPanEnv = te_env_present(voice->activePanEnvLoop);
    if (!(mayCarry && te_env_carry(voice->activePanEnvLoop))) {
        voice->envPanIndex = 0;
        voice->envPanTimeSec = 0.0;
        voice->envPan = voice->activePanEnv[0].value / 255.0;
    }
    if (!voice->hasPitchEnv) {
        voice->envPitchValue = 0.5; voice->envPitchIndex = 0; voice->envPitchTimeSec = 0.0;
    } else if (!(mayCarry && te_env_carry(voice->activePitchEnvLoop))) {
        voice->envPitchValue = te_seed_pf_role(voice->activePitchEnv, voice->activePitchEnvLoop,
            voice->activePitchEnvSustain, &voice->envPitchIndex, &voice->envPitchTimeSec);
    }
    if (!voice->hasFilterEnv) {
        voice->envFilterValue = 0.5; voice->envFilterIndex = 0; voice->envFilterTimeSec = 0.0;
    } else if (!(mayCarry && te_env_carry(voice->activeFilterEnvLoop))) {
        voice->envFilterValue = te_seed_pf_role(voice->activeFilterEnv, voice->activeFilterEnvLoop,
            voice->activeFilterEnvSustain, &voice->envFilterIndex, &voice->envFilterTimeSec);
    }
    voice->fadeoutVolume = 1.0;
    voice->rampOutSamples = 0;
    voice->rampOutGain = 0.0;
    /* Not on an OPL voice: its attack IS the chip's envelope. */
    voice->attackRampSamples = inst->opl != NULL ? 0 : e->attackRampSamples;
    voice->autoVibPhase = 0;
    voice->autoVibTicksSinceTrigger = 0;
    voice->nesDpcmCounter = 63;
    memset(&voice->right, 0, sizeof voice->right);
    voice->right.nesDpcmCounter = 63;
    voice->invertWritePos = 0;
    voice->funkWalk = -1;
    voice->funkPos = -1;
    voice->funkWindow = -1;
    voice->funkXfade = 0;
    voice->modFunkWindow = -1;
    voice->modFunkXfade = 0;
    voice->randomVolBias = inst->volumeSwing != 0
        ? (int)(te_random(e) * (2 * inst->volumeSwing + 1)) - inst->volumeSwing : 0;
    voice->randomPanBias = inst->panSwing != 0
        ? (int)(te_random(e) * (2 * inst->panSwing + 1)) - inst->panSwing : 0;

    seed->present = false;
    seed->pan = 0;
    seed->elevation = 0;
    if (instId != 0) {
        if (patch != NULL && patch->defaultPan != 0xff) {
            te_apply_note_pan_set(e, voice, patch->defaultPan);
            seed->present = true;
        } else if (((voice->activePanEnvLoop >> 7) & 1) != 0) {
            seed->present = true;
            if (e->ts_surroundModel == SURROUND_STEREO) {
                te_apply_note_pan_set(e, voice, inst->defaultPan);
            } else {
                te_apply_note_pan_set(e, voice, inst_default_azimuth(inst));
                te_apply_note_elevation(e, voice, inst_default_elevation(inst));
            }
        }
        if (inst->pitchPanSeparation != 0) {
            double noteDelta = (noteVal - inst->pitchPanCentre) / 4096.0;
            double panShift = trunc(noteDelta * inst->pitchPanSeparation * 4.0);
            te_apply_note_pan_slide(e, voice, panShift);
            seed->present = true;
        }
        seed->pan = voice->notePan;
        seed->elevation = voice->noteElevation;
    }
    voice->currentCutoff = voice->activeDefaultCutoff;
    voice->currentResonance = voice->activeDefaultResonance;
    voice->filterY1 = 0.0; voice->filterY2 = 0.0; voice->filterX1 = 0.0; voice->filterX2 = 0.0;
    voice->filterCutoffCached = -1;
    voice->filterResonanceCached = -1;
    voice->noteVal = noteVal;
    voice->basePitch = noteVal;
    voice->renderPitch = noteVal;
    voice->amigaPeriod = -1.0;
    voice->linearFreq = -1.0;
    voice->playbackRate = te_compute_playback_rate(e, voice, noteVal, e->tuningRatio);
    if (volOverride >= 0) voice->noteVolume = iclamp(volOverride, 0, e->volMax);
    else if (instId != 0) voice->noteVolume = te_row_volume_from_default(inst, patch, e->volMax);
    voice->rowVolume = voice->noteVolume;
    voice->snapMixVolume = true;
    voice->volRampSamples = 0;
    voice->volRampStep = 0.0;
    voice->snapPlaybackRate = true;
    voice->snapPan = true;
    voice->noteWasCut = false;
    voice->noteFading = false;
    voice->nnaOverride = -1;
    voice->volEnvOn = true;
    voice->panEnvOn = true;
    voice->pitchEnvOn = true;
    voice->filterEnvOn = true;
    voice->metaForeground = false;
    voice->metaForegroundDetune = 0;
    voice->fmRig = NULL;
    /* The OPL channel (item 209): keyed on here, carried or fresh. */
    if (inst->opl != NULL) {
        if (voice->opl == NULL) { voice->opl = &voice->oplStore; te_opl_state_init(voice->opl); }
        te_opl_key_on(voice->opl, inst->opl, oplCarry);
    } else {
        voice->opl = NULL;
    }
    /* A formant instrument (item 212): every trigger is a fresh syllable. */
    if (inst->formant != NULL) {
        if (voice->formant == NULL) { voice->formant = &voice->formantStore; te_formant_state_init(voice->formant); }
        te_formant_note_on(e, voice->formant, inst->formant);
    } else {
        voice->formant = NULL;
    }
    if (voice->vibratoRetrig) voice->vibratoLfoPos = 0;
    if (voice->tremoloRetrig) voice->tremoloLfoPos = 0;
    if (voice->panbrelloRetrig) voice->panbrelloLfoPos = 0;
}

/* What a trigger of (instId, note) sounds on the foreground voice: only an FM
 * rack shifts it, to operator 0's instrument and detune. */
static void principal_of(TaudEngine *e, int instId, int note, int *outInst, int *outNote)
{
    const TaudInst *inst = &e->instruments[instId];
    const TaudMetaLayer *op0;
    *outInst = instId;
    *outNote = note;
    if (!inst_is_fm(inst) || inst->meta->count == 0) return;
    op0 = &inst->meta->layers[0];
    if (op0->instIdx < 1 || op0->instIdx > 1023) return;
    *outInst = op0->instIdx;
    *outNote = iclamp(note + op0->detune, 0x20, 0xffff);
}

typedef struct { int newInstId, newNote; uint32_t newSmpPtr; int newSmpLen; } DupKey;

static bool is_duplicate(TaudEngine *e, const TaudVoice *v, const DupKey *k, bool bare)
{
    const TaudInst *existInst = &e->instruments[v->instrumentId];
    const TaudInst *newInst = &e->instruments[k->newInstId];
    /* A formant voice already into its coda, or ramping out, is no duplicate:
     * what the check ends is the VOWEL. */
    if (v->formant != NULL && (v->formant->phase >= PHASE_CODA || v->rampOutSamples > 0)) return false;
    if (bare && inst_dup_check_type(existInst) != DCT_LANE) return false;
    switch (inst_dup_check_type(existInst)) {
    case 1: return v->noteVal == k->newNote && v->instrumentId == k->newInstId;
    case 2:
        /* Formant instruments' "sample" is the pool range they are made from. */
        if (v->formant != NULL && existInst->formant != NULL && newInst->formant != NULL) {
            return existInst->formant->rangeLo == newInst->formant->rangeLo &&
                   existInst->formant->rangeHi == newInst->formant->rangeHi;
        }
        return v->instrumentId == k->newInstId && v->activeSamplePtr == k->newSmpPtr &&
               v->activeSampleLength == k->newSmpLen;
    case 3: return v->instrumentId == k->newInstId;
    case DCT_LANE: return true;
    default: return false;
    }
}

static TaudVoice *ghost_rig(TaudEngine *e, TaudVoice *voice, int channel);

static void apply_dup_action(TaudEngine *e, TaudVoice *v, bool isFg, int channel)
{
    const TaudInst *existInst = &e->instruments[v->instrumentId];
    switch (inst_dup_check_action(existInst)) {
    case 0:
        if (v->formant != NULL) {
            /* Ramped out over the incoming note's attack: the foreground voice
             * through a ghost, since the note is about to reuse it. */
            TaudVoice *r = isFg ? ghost_rig(e, v, channel) : v;
            if (r != NULL) te_start_cut_ramp(e, r);
            if (isFg) { v->active = false; te_cap_background_voices(e); }
            break;
        }
        v->fadeoutVolume = 0.0; v->active = false; break;
    case 1: v->keyOff = true; te_apply_key_lift(v, existInst); break;
    case 2: v->noteFading = true; break;
    default: break;
    }
}

void te_apply_duplicate_check(TaudEngine *e, int channel, int instId, int note)
{
    DupKey k;
    const TaudInst *newInst;
    const TaudPatch *newPatch;
    TaudVoice *fg;
    int i;
    /* Type 4, Lane (item 216): a note with no instrument byte is checked too,
     * against the lane's instrument; types 1-3 ask only about a named one. */
    bool bare = instId == 0;
    if (bare) instId = e->voices[channel].instrumentId;
    if (instId == 0) return;
    principal_of(e, instId, note, &k.newInstId, &k.newNote);
    newInst = &e->instruments[k.newInstId];
    newPatch = te_inst_resolve_patch(newInst, k.newNote, 0x3f);
    k.newSmpPtr = newPatch != NULL ? newPatch->samplePtr : newInst->samplePtr;
    k.newSmpLen = newPatch != NULL ? newPatch->sampleLength : newInst->sampleLength;

    fg = &e->voices[channel];
    if (fg->active && inst_dup_check_type(&e->instruments[fg->instrumentId]) != 0 && is_duplicate(e, fg, &k, bare)) {
        apply_dup_action(e, fg, true, channel);
    }
    for (i = e->bgCount - 1; i >= 0; i--) {
        TaudVoice *bg = e->bg[i];
        if (bg->sourceChannel != channel || !bg->active) continue;
        if (bg->fmOperator) continue;
        if (inst_dup_check_type(&e->instruments[bg->instrumentId]) == 0) continue;
        if (!is_duplicate(e, bg, &k, bare)) continue;
        apply_dup_action(e, bg, false, channel);
        if (!bg->active) te_bg_remove_at(e, i);
    }
}

/* Snapshot the playback-relevant state of src into a fresh background voice. */
static TaudVoice *ghost_voice(TaudEngine *e, const TaudVoice *src, int channel)
{
    TaudVoice *v = te_voice_alloc(e);
    if (v == NULL) return NULL;
    v->active = true;
    v->fader = src->fader;
    v->instrumentId = src->instrumentId;
    v->displayInst = src->displayInst;
    v->samplePos = src->samplePos;
    v->playbackRate = src->playbackRate;
    v->currentPlaybackRate = src->currentPlaybackRate;
    v->currentPan = src->currentPan;
    v->forward = src->forward;
    v->noteVolume = src->noteVolume;
    v->channelVolume = src->channelVolume;
    v->channelVolumeSet = src->channelVolumeSet;
    v->channelPanSet = src->channelPanSet;
    v->rowVolume = src->rowVolume;
    v->channelPan = src->channelPan;
    v->rowPan = src->rowPan;
    v->panAzimuth = src->panAzimuth;
    v->panElevation = src->panElevation;
    v->notePan = src->notePan;
    v->noteElevation = src->noteElevation;
    v->spatialTargetAz = src->spatialTargetAz;
    v->spatialTargetEl = src->spatialTargetEl;
    v->currentMixVolume = src->currentMixVolume;
    v->attackRampSamples = src->attackRampSamples;
    v->keyOff = src->keyOff;
    v->envIndex = src->envIndex;
    v->envTimeSec = src->envTimeSec;
    v->envVolume = src->envVolume;
    v->envVolMix = src->envVolMix;
    v->envVolStep = src->envVolStep;
    v->envPanIndex = src->envPanIndex;
    v->envPanTimeSec = src->envPanTimeSec;
    v->envPan = src->envPan;
    v->hasVolEnv = src->hasVolEnv;
    v->hasPanEnv = src->hasPanEnv;
    v->hasPitchEnv = src->hasPitchEnv;
    v->envPitchIndex = src->envPitchIndex;
    v->envPitchTimeSec = src->envPitchTimeSec;
    v->envPitchValue = src->envPitchValue;
    v->hasFilterEnv = src->hasFilterEnv;
    v->envFilterIndex = src->envFilterIndex;
    v->envFilterTimeSec = src->envFilterTimeSec;
    v->envFilterValue = src->envFilterValue;
    v->fadeoutVolume = src->fadeoutVolume;
    v->autoVibPhase = src->autoVibPhase;
    v->autoVibTicksSinceTrigger = src->autoVibTicksSinceTrigger;
    v->currentCutoff = src->currentCutoff;
    v->currentResonance = src->currentResonance;
    v->filterSfMode = src->filterSfMode;
    v->filterActive = src->filterActive;
    v->filterA0 = src->filterA0;
    v->filterB0 = src->filterB0;
    v->filterB1 = src->filterB1;
    v->filterY1 = src->filterY1;
    v->filterY2 = src->filterY2;
    v->filterIsBiquad = src->filterIsBiquad;
    v->filterBqB02 = src->filterBqB02;
    v->filterBqB1 = src->filterBqB1;
    v->filterBqA1 = src->filterBqA1;
    v->filterBqA2 = src->filterBqA2;
    v->filterX1 = src->filterX1;
    v->filterX2 = src->filterX2;
    v->filterCutoffCached = src->filterCutoffCached;
    v->filterResonanceCached = src->filterResonanceCached;
    v->randomVolBias = src->randomVolBias;
    v->randomPanBias = src->randomPanBias;
    v->panbrelloOffset = src->panbrelloOffset;
    v->noteVal = src->noteVal;
    v->basePitch = src->basePitch;
    v->amigaPeriod = src->amigaPeriod;
    v->linearFreq = src->linearFreq;
    v->volEnvOn = src->volEnvOn;
    v->panEnvOn = src->panEnvOn;
    v->pitchEnvOn = src->pitchEnvOn;
    v->filterEnvOn = src->filterEnvOn;
    v->metaForeground = src->metaForeground;
    v->metaForegroundDetune = src->metaForegroundDetune;
    v->noteFading = src->noteFading;
    v->layerMixGain = src->layerMixGain;
    v->layerRelPan = src->layerRelPan;
    v->layerRelElevation = src->layerRelElevation;
    v->clipMode = src->clipMode;
    v->bitcrusherDepth = src->bitcrusherDepth;
    v->bitcrusherSkip = src->bitcrusherSkip;
    v->bitcrusherCounter = src->bitcrusherCounter;
    v->bitcrusherHeld = src->bitcrusherHeld;
    v->overdriveAmp = src->overdriveAmp;
    v->sourceChannel = channel;
    v->activeSamplePtr = src->activeSamplePtr;
    v->activeSampleLength = src->activeSampleLength;
    v->activeSamplePlayStart = src->activeSamplePlayStart;
    v->activeSampleLoopStart = src->activeSampleLoopStart;
    v->activeSampleLoopEnd = src->activeSampleLoopEnd;
    v->activeSamplingRate = src->activeSamplingRate;
    v->activeSampleDetune = src->activeSampleDetune;
    v->activeLoopMode = src->activeLoopMode;
    v->funkWindow = src->funkWindow;
    v->modFunkWindow = src->modFunkWindow;
    v->activeVibratoSpeed = src->activeVibratoSpeed;
    v->activeVibratoSweep = src->activeVibratoSweep;
    v->activeVibratoDepth = src->activeVibratoDepth;
    v->activeVibratoRate = src->activeVibratoRate;
    v->activeVibratoWaveform = src->activeVibratoWaveform;
    v->activePatchIndex = src->activePatchIndex;
    v->activeChanCount = src->activeChanCount;
    v->activeChanMode = src->activeChanMode;
    v->activeChanPtr2 = src->activeChanPtr2;
    v->right = src->right;
    v->activeVolEnv = src->activeVolEnv;
    v->activeVolEnvLoop = src->activeVolEnvLoop;
    v->activeVolEnvSustain = src->activeVolEnvSustain;
    v->activePanEnv = src->activePanEnv;
    v->activePanEnvLoop = src->activePanEnvLoop;
    v->activePanEnvSustain = src->activePanEnvSustain;
    v->activePitchEnv = src->activePitchEnv;
    v->activePitchEnvLoop = src->activePitchEnvLoop;
    v->activePitchEnvSustain = src->activePitchEnvSustain;
    v->activeFilterEnv = src->activeFilterEnv;
    v->activeFilterEnvLoop = src->activeFilterEnvLoop;
    v->activeFilterEnvSustain = src->activeFilterEnvSustain;
    v->activeFadeoutStep = src->activeFadeoutStep;
    v->activeDefaultCutoff = src->activeDefaultCutoff;
    v->activeDefaultResonance = src->activeDefaultResonance;
    v->activeAttenGain = src->activeAttenGain;
    /* An OPL channel is the voice's whole sound and state: the ghost takes a
     * copy, clock and envelopes and all. A formant ghost keeps saying the
     * syllable it was saying, into its coda once released. */
    if (src->opl != NULL) { v->oplStore = src->oplStore; v->opl = &v->oplStore; }
    if (src->formant != NULL) { v->formantStore = src->formantStore; v->formant = &v->formantStore; }
    return v;
}

/* Ghost `voice` into the background pool — and, for an FM rack, its operands. */
static TaudVoice *ghost_rig(TaudEngine *e, TaudVoice *voice, int channel)
{
    TaudVoice *bg = ghost_voice(e, voice, channel);
    const TaudFmRig *rig;
    TaudFmRig *copy;
    int k;
    if (bg == NULL) return NULL;
    te_bg_push(e, bg);
    rig = voice->fmRig;
    if (rig == NULL) return bg;
    copy = &bg->rigStore;
    te_fm_rig_clone(copy, rig);
    bg->fmRig = copy;
    copy->voices[0] = bg;
    for (k = 1; k < rig->count; k++) {
        const TaudVoice *src = rig->voices[k];
        TaudVoice *op;
        if (src == NULL || !src->active) continue;
        op = ghost_voice(e, src, channel);
        if (op == NULL) break;
        op->isLayerChild = true;
        op->fmOperator = true;
        op->fmParent = bg;
        op->layerRelDetune = src->layerRelDetune;
        op->layerPitchMod = 0;
        op->layerMixGain = src->layerMixGain;
        copy->voices[k] = op;
        te_bg_push(e, op);
    }
    return bg;
}

void te_maybe_spawn_background_for_nna(TaudEngine *e, TaudVoice *voice, int channel, int incomingInst)
{
    int nna;
    TaudVoice *bg;
    if (!voice->active) return;
    nna = voice->nnaOverride >= 0 ? voice->nnaOverride : inst_new_note_action(&e->instruments[voice->instrumentId]);
    if (nna == 1 && voice->opl != NULL && voice->fmRig == NULL &&
        inst_is_opl(&e->instruments[incomingInst != 0 ? incomingInst : voice->instrumentId])) {
        /* An OPL note cut by an OPL note is a retrigger of the same channel
         * (§5.5.2): no ghost, no ramp; te_trigger_note picks it up. */
        voice->oplCarry = true;
        return;
    }
    if (nna == 1) {
        bg = ghost_rig(e, voice, channel);
        if (bg != NULL) te_start_cut_ramp(e, bg);
        te_cap_background_voices(e);
        return;
    }
    bg = ghost_rig(e, voice, channel);
    if (bg != NULL) {
        if (nna == 0 || nna == 4) {
            bg->keyOff = true;
            te_apply_key_lift(bg, &e->instruments[bg->instrumentId]);
        } else if (nna == 3) {
            bg->noteFading = true;
        }
    }
    te_cap_background_voices(e);
}

void te_apply_past_note_action(TaudEngine *e, int channel, int action)
{
    int i;
    switch (action) {
    case 0:
        for (i = e->bgCount - 1; i >= 0; i--) {
            if (e->bg[i]->sourceChannel == channel) te_bg_remove_at(e, i);
        }
        break;
    case 1:
        for (i = 0; i < e->bgCount; i++) {
            TaudVoice *bg = e->bg[i];
            if (bg->sourceChannel == channel) {
                bg->keyOff = true;
                te_apply_key_lift(bg, &e->instruments[bg->instrumentId]);
            }
        }
        break;
    case 2:
        for (i = 0; i < e->bgCount; i++) {
            if (e->bg[i]->sourceChannel == channel) e->bg[i]->noteFading = true;
        }
        break;
    default:
        break;
    }
}

void te_apply_vol_column(TaudEngine *e, TaudVoice *v, int value, int sel)
{
    int dirBit = e->ts_wideCells ? 0x80 : 0x20;
    switch (sel) {
    case 0:
        v->noteVolume = iclamp(value, 0, e->volMax);
        v->rowVolume = v->noteVolume;
        break;
    case 1: v->volColSlideUp = value; break;
    case 2: v->volColSlideDown = value; break;
    case 3: {
        int mag;
        if (value == 0) return;
        mag = value & (dirBit - 1);
        v->noteVolume = (value & dirBit) != 0 ? imin(v->noteVolume + mag, e->volMax) : imax(v->noteVolume - mag, 0);
        v->rowVolume = v->noteVolume;
        break;
    }
    default: break;
    }
}

void te_apply_pan_column(TaudEngine *e, TaudVoice *v, int value, int sel)
{
    switch (sel) {
    case 0:
        te_apply_note_pan_set(e, v, (value << 2) | (value >> 4));
        break;
    case 1: v->panColSlideRight = value; break;
    case 2: v->panColSlideLeft = value; break;
    case 3: {
        int mag;
        if (value == 0) return;
        mag = value & 0x1f;
        te_apply_note_pan_slide(e, v, (value & 0x20) != 0 ? mag : -mag);
        break;
    }
    default: break;
    }
}

static bool row_slides_spatially(const TaudCell *row)
{
    return (row->effect == OP_Z && (row->effectArg & 0xfff) != 0) ||
           (row->effect2 == OP_Z && (row->effectArg2 & 0xfff) != 0);
}

void te_apply_pan_column_wide(TaudEngine *e, TaudVoice *v, const TaudCell *row)
{
    switch (row->panEff) {
    case 0:
        if (row_slides_spatially(row)) {
            v->spatialTargetAz = row->azimuth;
            v->spatialTargetEl = e->ts_surroundModel == SURROUND_SPATIAL ? row->elevation : 0;
        } else {
            te_apply_note_pan_set(e, v, row->azimuth);
            te_apply_note_elevation(e, v, row->elevation);
        }
        break;
    case 1: v->panColSlideRight = row->azimuth & 0xff; break;
    case 2: v->panColSlideLeft = row->azimuth & 0xff; break;
    case 3: {
        int mag = row->azimuth & 0xff;
        if (mag == 0) return;
        te_apply_note_pan_slide(e, v, (row->azimuth & 0x100) != 0 ? mag : -mag);
        break;
    }
    default: break;
    }
}
