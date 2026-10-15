/* Mixer + output quantiser — port of mixer.js.
 *
 * The voices sum in binary64; the pair narrows to binary32 (the mix bus)
 * exactly once per frame, then mastering, clamp and (optionally) dither. */

#include "taud_internal.h"

#define PI 3.141592653589793

/* One voice's contribution as a [left, right] pair, before pan and gain. */
static void render_voice_pair(TaudEngine *e, TaudVoice *v, TaudInst *inst, int interpMode, double spt, double out[2])
{
    double s, c0, c1;
    if (v->fmRig != NULL) {
        s = te_render_fm_voice(e, v, interpMode, spt);
        s = te_apply_voice_filter_self(v, s);
        s = te_apply_taud_voice_fx(v, s, &v->bitcrusherCounter, &v->bitcrusherHeld);
        out[0] = s;
        out[1] = s;
        return;
    }
    if (v->activeChanCount != 2) {
        s = te_fetch_tracker_sample(e, v, inst, interpMode, 0);
        s = te_apply_voice_filter_self(v, s);
        s = te_apply_taud_voice_fx(v, s, &v->bitcrusherCounter, &v->bitcrusherHeld);
        out[0] = s;
        out[1] = s;
        return;
    }
    te_fetch_tracker_sample_stereo(e, v, inst, interpMode, out);
    c0 = out[0];
    c1 = out[1];
    if (v->activeChanMode == CHAN_MODE_MATRIX) {
        double m = c0, sd = c1;
        c0 = m + sd;
        c1 = m - sd;
    }
    out[0] = te_apply_taud_voice_fx(v, te_apply_voice_filter_self(v, c0), &v->bitcrusherCounter, &v->bitcrusherHeld);
    out[1] = te_apply_taud_voice_fx(v, te_apply_voice_filter(v, c1, &v->right),
                                    &v->right.bitcrusherCounter, &v->right.bitcrusherHeld);
}

/* ── pcm32fToPcm8: noise-shaped TPDF dither, binary32 at every step ── */

static uint32_t xorshift32(TaudEngine *e)
{
    uint32_t x = e->xorshift;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    e->xorshift = x;
    return x;
}

static float urand(TaudEngine *e) { return (float)((xorshift32(e) & 0xffffff) / 16777216.0); }
static float tpdf1(TaudEngine *e) { float a = urand(e); float b = urand(e); return a - b; }

void te_pcm32f_to_pcm8(TaudEngine *e, uint8_t *out)
{
    const float b1 = 1.5f, b2 = -0.75f, scale = 127.5f, ditherScale = 0.2f;
    float *err = e->ditherError;
    int i;
    for (i = 0; i < TRACKER_CHUNK; i++) {
        float feedback, dither, shaped, qerr;
        int q;
        /* left */
        feedback = (float)((float)(b1 * err[0]) + (float)(b2 * err[1]));
        dither = (float)(ditherScale * tpdf1(e));
        shaped = (float)((float)(e->mixLeft[i] + feedback) + (float)(dither / scale));
        shaped = shaped < -1.0f ? -1.0f : shaped > 1.0f ? 1.0f : shaped;
        q = (int)js_round((float)(shaped * scale));
        q = q < -128 ? -128 : q > 127 ? 127 : q;
        out[i * 2] = (uint8_t)((q + 128) & 0xff);
        qerr = (float)(shaped - (float)(q / scale));
        err[1] = err[0];
        err[0] = qerr;
        /* right */
        feedback = (float)((float)(b1 * err[2]) + (float)(b2 * err[3]));
        dither = (float)(ditherScale * tpdf1(e));
        shaped = (float)((float)(e->mixRight[i] + feedback) + (float)(dither / scale));
        shaped = shaped < -1.0f ? -1.0f : shaped > 1.0f ? 1.0f : shaped;
        q = (int)js_round((float)(shaped * scale));
        q = q < -128 ? -128 : q > 127 ? 127 : q;
        out[i * 2 + 1] = (uint8_t)((q + 128) & 0xff);
        qerr = (float)(shaped - (float)(q / scale));
        err[3] = err[2];
        err[2] = qerr;
    }
}

static inline double attack_gain(const TaudEngine *e, int attackRampSamples)
{
    int elapsed = e->attackRampSamples - attackRampSamples;
    return 0.5 - 0.5 * js_cos((PI * elapsed) / e->attackRampSamples);
}

/* Render one TRACKER_CHUNK into mixLeft/mixRight. */
bool te_generate_tracker_audio(TaudEngine *e)
{
    bool advancing = e->isPlaying;
    TaudSpatialBus *spatial = e->spatial;
    double srate = e->rate;
    int n, vi, i;

    if (spatial != NULL) {
        int c;
        for (c = 0; c < spatial->numChannels * TRACKER_CHUNK; c++) spatial->data[c] = 0.0;
    }

    if (advancing && e->firstRow) {
        e->firstRow = false;
        te_apply_tracker_row(e);
    }

    for (n = 0; n < TRACKER_CHUNK; n++) {
        double spt = (srate * 2.5) / e->bpm;
        double mixL = 0.0, mixR = 0.0;
        double gvol, mvol, globalGain;

        if (advancing) {
            e->samplesIntoTick += 1.0;
            if (e->samplesIntoTick >= spt) {
                e->samplesIntoTick -= spt;
                te_apply_tracker_tick(e);
                e->tickInRow++;
                if (e->tickInRow >= e->tickRate + e->finePatternDelayExtra) {
                    e->tickInRow = 0;
                    te_advance_row(e);
                }
            }
        } else {
            e->samplesIntoTick += 1.0;
            if (e->samplesIntoTick >= spt) {
                e->samplesIntoTick -= spt;
                te_apply_tracker_tick(e);
            }
        }

        gvol = e->globalVolume / 255.0;
        mvol = e->mixingVolume / 255.0;
        globalGain = (gvol * mvol * e->masterVolume) / 255.0;

        for (vi = 0; vi < MAX_VOICES; vi++) {
            TaudVoice *voice = &e->voices[vi];
            TaudInst *inst;
            double pair[2], sL, sR, instGv, swingScale, effEnvVol, faderGain, perVoiceGain, vol;
            double lGain = 0.0, rGain = 0.0, rampGain;
            if (!voice->active || voice->fader == 255) continue;
            inst = &e->instruments[voice->instrumentId];
            te_advance_sample_mod_extended(e, voice, spt);
            render_voice_pair(e, voice, inst, e->interpolationMode, spt, pair);
            sL = pair[0];
            sR = pair[1];
            instGv = inst->instGlobalVolume / 255.0;
            swingScale = 1.0 + voice->randomVolBias / 255.0;
            voice->envVolMix += voice->envVolStep;
            effEnvVol = voice->hasVolEnv && voice->volEnvOn ? voice->envVolMix : 1.0;
            te_advance_volume_ramp(e, voice, e->volDiv);
            te_advance_pitch_ramp(e, voice, spt);
            faderGain = (255 - voice->fader) / 255.0;
            perVoiceGain = effEnvVol * voice->fadeoutVolume * voice->currentMixVolume *
                swingScale * instGv * faderGain * voice->layerMixGain * voice->activeAttenGain;
            vol = perVoiceGain * globalGain;
            if (spatial == NULL) {
                double pan = te_advance_pan_ramp(e, voice, te_voice_pan_byte(voice), false);
                if (pan != voice->panLawPan) {
                    voice->panLawPan = pan;
                    voice->panLawL = js_cos((PI * pan) / 512.0);
                    voice->panLawR = js_sin((PI * pan) / 512.0);
                }
                lGain = voice->panLawL;
                rGain = voice->panLawR;
            } else {
                te_advance_pan_ramp(e, voice, te_voice_azimuth(voice), true);
            }
            if (voice->rampOutSamples > 0) {
                rampGain = voice->rampOutGain;
                voice->rampOutGain -= voice->rampOutStep;
                voice->rampOutSamples--;
                if (voice->rampOutSamples == 0) voice->active = false;
            } else {
                rampGain = 1.0;
            }
            if (voice->attackRampSamples > 0) {
                rampGain *= attack_gain(e, voice->attackRampSamples);
                voice->attackRampSamples--;
            }
            if (spatial == NULL) {
                mixL += sL * vol * lGain * rampGain;
                mixR += sR * vol * rGain * rampGain;
            } else {
                const double *g = te_spatial_voice_gains(e, voice);
                te_spatial_bus_add_source(spatial, n, sL * vol, g, 0, rampGain);
                if (voice->activeChanCount == 2) {
                    te_spatial_bus_add_source(spatial, n, sR * vol, g, spatial->numChannels, rampGain);
                }
            }
        }

        for (i = 0; i < e->bgCount; i++) {
            TaudVoice *bg = e->bg[i];
            TaudVoice *srcVoice;
            TaudInst *inst;
            int bgFader;
            double pair[2], sL, sR, instGv, swingScale, effEnvVol, faderGain, vol;
            double lGain = 0.0, rGain = 0.0, rampGain;
            if (bg->fmOperator) continue;
            srcVoice = bg->sourceChannel >= 0 && bg->sourceChannel < MAX_VOICES ? &e->voices[bg->sourceChannel] : NULL;
            bgFader = srcVoice != NULL && srcVoice->fader > bg->fader ? srcVoice->fader : bg->fader;
            if (!bg->active || bgFader == 255) continue;
            inst = &e->instruments[bg->instrumentId];
            if (bg->isLayerChild) te_advance_sample_mod_extended(e, bg, spt);
            render_voice_pair(e, bg, inst, e->interpolationMode, spt, pair);
            sL = pair[0];
            sR = pair[1];
            instGv = inst->instGlobalVolume / 255.0;
            swingScale = 1.0 + bg->randomVolBias / 255.0;
            bg->envVolMix += bg->envVolStep;
            effEnvVol = bg->hasVolEnv && bg->volEnvOn ? bg->envVolMix : 1.0;
            te_advance_volume_ramp(e, bg, e->volDiv);
            te_advance_pitch_ramp(e, bg, spt);
            faderGain = (255 - bgFader) / 255.0;
            vol = (effEnvVol * bg->fadeoutVolume * bg->currentMixVolume *
                   swingScale * gvol * mvol * instGv * faderGain * bg->layerMixGain * bg->activeAttenGain *
                   e->masterVolume) / 255.0;
            if (spatial == NULL) {
                double pan = te_advance_pan_ramp(e, bg, te_voice_pan_byte(bg), false);
                if (pan != bg->panLawPan) {
                    bg->panLawPan = pan;
                    bg->panLawL = js_cos((PI * pan) / 512.0);
                    bg->panLawR = js_sin((PI * pan) / 512.0);
                }
                lGain = bg->panLawL;
                rGain = bg->panLawR;
            } else {
                te_advance_pan_ramp(e, bg, te_voice_azimuth(bg), true);
            }
            if (bg->rampOutSamples > 0) {
                rampGain = bg->rampOutGain;
                bg->rampOutGain -= bg->rampOutStep;
                bg->rampOutSamples--;
                if (bg->rampOutSamples == 0) bg->active = false;
            } else {
                rampGain = 1.0;
            }
            if (bg->attackRampSamples > 0) {
                rampGain *= attack_gain(e, bg->attackRampSamples);
                bg->attackRampSamples--;
            }
            if (spatial == NULL) {
                mixL += sL * vol * lGain * rampGain;
                mixR += sR * vol * rGain * rampGain;
            } else {
                const double *g = te_spatial_voice_gains(e, bg);
                te_spatial_bus_add_source(spatial, n, sL * vol, g, 0, rampGain);
                if (bg->activeChanCount == 2) {
                    te_spatial_bus_add_source(spatial, n, sR * vol, g, spatial->numChannels, rampGain);
                }
            }
        }

        if (spatial != NULL) {
            double pair[2];
            te_spatial_bus_stereo_at(spatial, n, pair);
            mixL = pair[0];
            mixR = pair[1];
        }

        if (e->interpolationMode == INTERP_A500) {
            e->amigaLPStateL = mixL * e->a500A0 + e->amigaLPStateL * e->a500B1;
            e->amigaLPStateR = mixR * e->a500A0 + e->amigaLPStateR * e->a500B1;
            mixL = e->amigaLPStateL;
            mixR = e->amigaLPStateR;
        }
        if ((e->interpolationMode == INTERP_A500 || e->interpolationMode == INTERP_A1200) && e->ledFilterOn) {
            double *sl = e->amigaLEDStateL, *sr = e->amigaLEDStateR;
            double outL = mixL * e->ledA1 + sl[0] * e->ledA2 + sl[1] * e->ledA1 - sl[2] * e->ledB1 - sl[3] * e->ledB2;
            double outR = mixR * e->ledA1 + sr[0] * e->ledA2 + sr[1] * e->ledA1 - sr[2] * e->ledB1 - sr[3] * e->ledB2;
            sl[1] = sl[0]; sl[0] = mixL; sl[3] = sl[2]; sl[2] = outL;
            sr[1] = sr[0]; sr[0] = mixR; sr[3] = sr[2]; sr[2] = outR;
            mixL = outL;
            mixR = outR;
        }

        e->mixLeft[n] = (float)mixL;
        e->mixRight[n] = (float)mixR;
    }

    if (e->mastering != NULL) te_master_chain_process(e->mastering, e->mixLeft, e->mixRight, TRACKER_CHUNK);
    for (n = 0; n < TRACKER_CHUNK; n++) {
        float fl = e->mixLeft[n], fr = e->mixRight[n];
        if (fl < -1.0f) e->mixLeft[n] = -1.0f; else if (fl > 1.0f) e->mixLeft[n] = 1.0f;
        if (fr < -1.0f) e->mixRight[n] = -1.0f; else if (fr > 1.0f) e->mixRight[n] = 1.0f;
    }

    /* A halt cue clears isPlaying mid-chunk: end the song's voices here. */
    if (advancing && !e->isPlaying) te_silence_song_voices(e, false);
    return true;
}
