/* Surround and ambisonics core — port of spatial.js (the parts playback uses:
 * the stereo fold renderer, the per-voice position model, the Z slide, and the
 * object bus the binaural monitor decodes). */

#include "taud_internal.h"

#define PI 3.141592653589793
#define AZ_TO_RAD ((2 * PI) / AZIMUTH_TURN)
#define EL_TO_RAD (PI / 2 / ELEVATION_QUARTER)
#define AZ_PER_DEG (AZIMUTH_TURN / 360.0)
#define AZ_FRONT 128

double te_wrap_azimuth(double a)
{
    double r = fmod(a, AZIMUTH_TURN);
    return r < 0 ? r + AZIMUTH_TURN : r;
}

double te_fold_azimuth_to_pan(double az)
{
    double a = te_wrap_azimuth(az);
    double p = a <= 256 ? a : AZIMUTH_TURN - a;
    return p > 255 ? 255 : p;
}

void te_direction_from_angles(double az, double el, double out[3])
{
    double th = (AZ_FRONT - az) * AZ_TO_RAD;
    double ph = el * EL_TO_RAD;
    double cph = js_cos(ph);
    out[0] = cph * js_cos(th);
    out[1] = cph * js_sin(th);
    out[2] = js_sin(ph);
}

void te_angles_from_direction(double x, double y, double z, double out[2])
{
    out[0] = te_wrap_azimuth(AZ_FRONT - js_atan2(y, x) / AZ_TO_RAD);
    out[1] = js_asin(dclamp(z, -1.0, 1.0)) / EL_TO_RAD;
}

/* ITU placement of a sample's channels as azimuth offsets (stereo = ±30°). */
static double sample_channel_layout(int chans, int k)
{
    if (chans == 2) return (k == 0 ? -30 : 30) * AZ_PER_DEG;
    return 0 * AZ_PER_DEG;
}

void te_sample_channel_angles(double az, double el, double localAz, double out[2])
{
    double psi, th, ph, cpsi, spsi, cph, sph, cth, sth;
    if (localAz == 0) { out[0] = az; out[1] = el; return; }
    if (el == 0) { out[0] = az + localAz; out[1] = 0; return; }
    psi = -localAz * AZ_TO_RAD;
    th = (AZ_FRONT - az) * AZ_TO_RAD;
    ph = el * EL_TO_RAD;
    cpsi = js_cos(psi); spsi = js_sin(psi);
    cph = js_cos(ph); sph = js_sin(ph);
    cth = js_cos(th); sth = js_sin(th);
    te_angles_from_direction(cpsi * cph * cth - spsi * sth,
                             cpsi * cph * sth + spsi * cth,
                             cpsi * sph, out);
}

void te_angles_from_spatial_arg(int arg, double out[2])
{
    int ee = (arg >> 8) & 0xff;
    out[0] = (arg & 0xff) * 2;
    out[1] = ee >= 0x80 ? ee - 256 : ee;
}

/* Math.hypot(a, b, c) exactly as V8 computes it: scale by the largest
 * magnitude, Kahan-sum the squares, un-scale. */
static double js_hypot3(double a, double b, double c)
{
    double v[3], max = 0, sum = 0, comp = 0;
    int i;
    v[0] = fabs(a); v[1] = fabs(b); v[2] = fabs(c);
    for (i = 0; i < 3; i++) if (v[i] > max) max = v[i];
    if (max == 0) return 0;
    for (i = 0; i < 3; i++) {
        double n = v[i] / max;
        double summand = n * n - comp;
        double prelim = sum + summand;
        comp = (prelim - sum) - summand;
        sum = prelim;
    }
    return sqrt(sum) * max;
}

/* One tick of a Z slide: rotate toward the target along the great circle. */
void te_step_toward_target(double az, double el, double tgtAz, double tgtEl, double stepUnits, double out[2])
{
    double a[3], b[3], dot, omega, step, vx, vy, vz;
    te_direction_from_angles(az, el, a);
    te_direction_from_angles(tgtAz, tgtEl, b);
    dot = dclamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0);
    omega = js_acos(dot);
    step = stepUnits * AZ_TO_RAD;
    if (!(omega > 1e-12) || step <= 0.0) { out[0] = az; out[1] = el; return; }
    if (step >= omega) { out[0] = te_wrap_azimuth(tgtAz); out[1] = tgtEl; return; }

    if (omega > PI - 1e-9) {
        double kx = a[2] * a[0];
        double ky = a[2] * a[1];
        double kz = a[2] * a[2] - 1.0;
        double len = js_hypot3(kx, ky, kz);
        double c, s;
        if (len < 1e-9) {
            double th = (AZ_FRONT - az) * AZ_TO_RAD;
            double hx = js_cos(th), hy = js_sin(th);
            kx = -a[2] * hy;
            ky = a[2] * hx;
            kz = a[0] * hy - a[1] * hx;
            len = js_hypot3(kx, ky, kz);
        }
        kx /= len; ky /= len; kz /= len;
        c = js_cos(step); s = js_sin(step);
        vx = a[0] * c + (ky * a[2] - kz * a[1]) * s;
        vy = a[1] * c + (kz * a[0] - kx * a[2]) * s;
        vz = a[2] * c + (kx * a[1] - ky * a[0]) * s;
    } else {
        double sinOmega = js_sin(omega);
        double c0 = js_sin(omega - step) / sinOmega;
        double c1 = js_sin(step) / sinOmega;
        vx = a[0] * c0 + b[0] * c1;
        vy = a[1] * c0 + b[1] * c1;
        vz = a[2] * c0 + b[2] * c1;
    }
    te_angles_from_direction(vx, vy, vz, out);
}

/* Real spherical harmonics up to `order` (≤ 3), SN3D, ACN order. */
void te_encode_sn3d(double az, double el, int order, double *out)
{
    const double SQRT3 = sqrt(3.0);
    const double SQRT15 = sqrt(15.0);
    const double SQRT5_8 = sqrt(5.0 / 8.0);
    const double SQRT3_8 = sqrt(3.0 / 8.0);
    double th = (AZ_FRONT - az) * AZ_TO_RAD;
    double ph = el * EL_TO_RAD;
    double cph = js_cos(ph);
    double x = cph * js_cos(th);
    double y = cph * js_sin(th);
    double z = js_sin(ph);

    out[0] = 1.0;
    if (order < 1) return;
    out[1] = y;
    out[2] = z;
    out[3] = x;
    if (order < 2) return;
    out[4] = SQRT3 * x * y;
    out[5] = SQRT3 * y * z;
    out[6] = (3.0 * z * z - 1.0) * 0.5;
    out[7] = SQRT3 * x * z;
    out[8] = SQRT3 * (x * x - y * y) * 0.5;
    if (order < 3) return;
    out[9] = SQRT5_8 * y * (3.0 * x * x - y * y);
    out[10] = SQRT15 * x * y * z;
    out[11] = SQRT3_8 * y * (5.0 * z * z - 1.0);
    out[12] = z * (5.0 * z * z - 3.0) * 0.5;
    out[13] = SQRT3_8 * x * (5.0 * z * z - 1.0);
    out[14] = SQRT15 * z * (x * x - y * y) * 0.5;
    out[15] = SQRT5_8 * x * (x * x - 3.0 * y * y);
}

/* ── bus ── */

static void stereo_channel_gains(double az, double el, double *out, int off)
{
    double p = te_fold_azimuth_to_pan(az);
    double pan = el == 0 ? p : 128 + (p - 128) * js_cos(el * EL_TO_RAD);
    out[off] = js_cos((PI * pan) / 512.0);
    out[off + 1] = js_sin((PI * pan) / 512.0);
}

static void bus_channel_gains(const TaudSpatialBus *bus, double az, double el, double *out, int off)
{
    if (bus->kind == 1) te_binaural_channel_gains(bus->bin, az, el, out, off);
    else stereo_channel_gains(az, el, out, off);
}

void te_spatial_bus_add_source(TaudSpatialBus *bus, int n, double value, const double *gains, int off, double ramp)
{
    int c, nc = bus->numChannels;
    for (c = 0; c < nc; c++) bus->data[c * TRACKER_CHUNK + n] += (value * gains[off + c]) * ramp;
}

void te_spatial_bus_stereo_at(TaudSpatialBus *bus, int n, double out[2])
{
    if (bus->kind == 1) {
        te_binaural_monitor_stereo(bus->bin, bus->data, TRACKER_CHUNK, n, out);
    } else {
        out[0] = bus->data[n];
        out[1] = bus->data[TRACKER_CHUNK + n];
    }
}

/* ── per-voice spatial state ── */

static double mirror_pan_byte(double az) { return js_round(te_fold_azimuth_to_pan(az)); }

void te_apply_pan_set(TaudEngine *e, TaudVoice *v, double pan)
{
    v->channelPanSet = true;
    if (e->ts_surroundModel == SURROUND_STEREO) {
        v->channelPan = js_toint32(pan) & 0xff;
    } else {
        v->panAzimuth = te_wrap_azimuth(pan);
        v->channelPan = (int)mirror_pan_byte(v->panAzimuth);
    }
    v->rowPan = iclamp((int)((unsigned)v->channelPan >> 2), 0, 63);
}

void te_apply_pan_slide(TaudEngine *e, TaudVoice *v, double delta)
{
    v->channelPanSet = true;
    if (e->ts_surroundModel == SURROUND_STEREO) {
        v->channelPan = delta < 0 ? (int)dmax(v->channelPan + delta, 0)
                                  : (int)dmin(v->channelPan + delta, 0xff);
    } else {
        v->panAzimuth = te_wrap_azimuth(v->panAzimuth + delta);
        v->channelPan = (int)mirror_pan_byte(v->panAzimuth);
    }
    v->rowPan = iclamp((int)((unsigned)v->channelPan >> 2), 0, 63);
}

void te_apply_elevation(TaudEngine *e, TaudVoice *v, double el)
{
    v->panElevation = e->ts_surroundModel == SURROUND_SPATIAL ? el : 0.0;
}

double te_bound_note_pan(const TaudEngine *e, double off)
{
    if (e->ts_surroundModel == SURROUND_STEREO) return dclamp(off, -0xff, 0xff);
    return te_wrap_azimuth(off + AZIMUTH_TURN / 2) - AZIMUTH_TURN / 2;
}

void te_apply_note_pan_set(TaudEngine *e, TaudVoice *v, double pan)
{
    v->notePan = te_bound_note_pan(e, pan - AZ_FRONT);
}

void te_apply_note_pan_slide(TaudEngine *e, TaudVoice *v, double delta)
{
    v->notePan = te_bound_note_pan(e, v->notePan + delta);
}

void te_apply_note_elevation(TaudEngine *e, TaudVoice *v, double el)
{
    v->noteElevation = e->ts_surroundModel == SURROUND_SPATIAL ? el : 0.0;
}

static double env_pan_raw(const TaudVoice *v)
{
    double r = js_round(v->envPan * 255.0);
    return r < 0 ? 0 : r > 255 ? 255 : r;
}

double te_voice_azimuth(const TaudVoice *v)
{
    if (v->hasPanEnv && v->panEnvOn) {
        return te_wrap_azimuth(v->panAzimuth + v->notePan + env_pan_raw(v) - 128 + v->randomPanBias +
                               v->panbrelloOffset);
    }
    return te_wrap_azimuth(v->panAzimuth + v->notePan + v->randomPanBias + v->panbrelloOffset);
}

double te_voice_pan_byte(const TaudVoice *v)
{
    double pan;
    if (v->hasPanEnv && v->panEnvOn) {
        pan = v->channelPan + v->notePan + env_pan_raw(v) - 128 + v->randomPanBias + v->panbrelloOffset;
    } else {
        pan = v->channelPan + v->notePan + v->randomPanBias + v->panbrelloOffset;
    }
    return pan < 0 ? 0 : pan > 255 ? 255 : pan;
}

double te_voice_elevation(const TaudVoice *v) { return v->panElevation + v->noteElevation; }

bool te_is_sounding_child(const TaudEngine *e, const TaudVoice *bg, int vi)
{
    if (!bg->isLayerChild || bg->sourceChannel != vi) return false;
    return !bg->fmOperator || bg->fmParent == &e->voices[vi];
}

static double display_weight(const TaudVoice *v)
{
    double env = v->volEnvOn ? v->envVolMix : 1.0;
    return env * v->fadeoutVolume * v->currentMixVolume * v->layerMixGain * ((255 - v->fader) / 255.0);
}

/* The stereo pan the meters show for lane `vi`: the mix-weighted mean of the
 * foreground voice and its layer children. */
double te_display_pan_byte(TaudEngine *e, int vi, const TaudVoice *v)
{
    double sum = 0.0, wsum = 0.0, w;
    int i;
    w = display_weight(v);
    sum += te_voice_pan_byte(v) * w;
    wsum += w;
    if (v->metaForeground) {
        for (i = 0; i < e->bgCount; i++) {
            const TaudVoice *bg = e->bg[i];
            if (bg->active && te_is_sounding_child(e, bg, vi)) {
                w = display_weight(bg);
                sum += te_voice_pan_byte(bg) * w;
                wsum += w;
            }
        }
    }
    return wsum > 1e-9 ? sum / wsum : te_voice_pan_byte(v);
}

/* Renderer gains for every channel of `v`, cached until the source moves. */
const double *te_spatial_voice_gains(TaudEngine *e, TaudVoice *v)
{
    TaudSpatialBus *bus = e->spatial;
    TaudSpatialCache *sc = &v->spatial;
    int nc = bus->numChannels;
    int chans = v->activeChanCount == 2 ? 2 : 1;
    double az = v->currentPan;
    double el = te_voice_elevation(v);
    if (!sc->valid || sc->az != az || sc->el != el || sc->chans != chans || sc->rendererGen != e->rendererGen) {
        int k;
        for (k = 0; k < chans; k++) {
            double ang[2];
            te_sample_channel_angles(az, el, sample_channel_layout(chans, k), ang);
            bus_channel_gains(bus, ang[0], ang[1], sc->gains, k * nc);
        }
        sc->valid = true;
        sc->az = az; sc->el = el; sc->chans = chans; sc->rendererGen = e->rendererGen;
    }
    return sc->gains;
}
