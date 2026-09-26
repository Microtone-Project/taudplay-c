/* .taud container parser — port of taud-parse.js, taud-const.js and
 * offline-render.js loadIntoEngine. */

#include <stdlib.h>
#include <string.h>

#include "taud_internal.h"

#define TAUD_HEADER_SIZE 32
#define TAUD_SONG_ENTRY 32
#define TAUD_VERSION_MASK 0x1f
#define TAUD_XHDR_FLAG 0x20
#define TAUD_KIND_MASK 0xc0
#define TAUD_KIND_FULL 0x00
#define TAUD_KIND_SAMPLEINST 0x80
#define TAUD_KIND_PATTERN 0xc0
#define CUE_SIZE 64
#define CUE_SIZE_64 128
#define NUM_CUES_64 4096
#define CUE_EMPTY 0x7fff
#define CUE_SIZE_V1 32
#define NUM_VOICES_V1 20
#define CUE_EMPTY_V1 0xfff

static const uint8_t TAUD_MAGIC[8] = { 0x1f, 0x54, 0x53, 0x56, 0x4d, 0x61, 0x75, 0x64 };
static const uint8_t PROJ_MAGIC[8] = { 0x1e, 0x54, 0x61, 0x75, 0x64, 0x50, 0x72, 0x4a };

static uint32_t rd_u16(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8); }
static uint32_t rd_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static float rd_f32(const uint8_t *b)
{
    union { uint32_t u; float f; } c;
    c.u = rd_u32(b);
    return c.f;
}

static char *str_nul(const uint8_t *b, size_t o, size_t end, size_t *next)
{
    size_t s = o, n;
    char *out;
    while (o < end && b[o] != 0) o++;
    n = o - s;
    out = (char *)malloc(n + 1);
    if (out != NULL) { memcpy(out, b + s, n); out[n] = 0; }
    if (next != NULL) *next = o + 1;
    return out;
}

static int ixmp_patch_len(const uint8_t *b, size_t o, size_t len)
{
    int ver = b[o];
    int base = 31 + ((ver & 0x80) ? 15 : 0) + ((ver & 0x02) ? 54 : 0) + ((ver & 0x04) ? 54 : 0) +
               ((ver & 0x08) ? 54 : 0) + ((ver & 0x10) ? 54 : 0);
    if ((ver & 0x20) == 0) return base;
    {
        int chanByte = o + (size_t)base < len ? b[o + base] : 0;
        return base + 4 + 4 * (chanByte >> 4);
    }
}

/* _v1CueToV2: 20 lanes × 12-bit patterns in nibble planes + an instruction word. */
static void v1_cue_to_words(const uint8_t *b, uint16_t *out)
{
    int word0 = (b[30] << 8) | b[31], ch;
    for (ch = 0; ch < MAX_VOICES; ch++) out[ch] = CUE_EMPTY;
    for (ch = 0; ch < NUM_VOICES; ch++) {
        int pat = CUE_EMPTY, val;
        if (ch < NUM_VOICES_V1) {
            int bi = ch >> 1;
            int lo = (ch & 1) ? (b[bi] & 0xf) : ((b[bi] >> 4) & 0xf);
            int mi = (ch & 1) ? (b[10 + bi] & 0xf) : ((b[10 + bi] >> 4) & 0xf);
            int hi = (ch & 1) ? (b[20 + bi] & 0xf) : ((b[20 + bi] >> 4) & 0xf);
            int p12 = (hi << 8) | (mi << 4) | lo;
            pat = p12 == CUE_EMPTY_V1 ? CUE_EMPTY : p12;
        }
        val = pat & 0x7fff;
        if (ch < 16 && ((word0 >> ch) & 1)) val |= 0x8000;
        out[ch] = (uint16_t)val;
    }
}

void te_doc_free(TaudDoc *doc)
{
    int i;
    if (doc == NULL) return;
    free(doc->sampleInstImage);
    for (i = 0; i < doc->numSongs; i++) {
        free(doc->songs[i].patBin);
        free(doc->songs[i].cues);
        free(doc->songs[i].name);
    }
    free(doc->songs);
    free(doc->projData);
    free(doc->ixmp);
    free(doc->projectName);
    memset(doc, 0, sizeof *doc);
}

typedef struct { char fourcc[4]; size_t off, len; } Section;

static const Section *find_sec(const Section *secs, int n, const char *fc)
{
    int i;
    for (i = 0; i < n; i++) if (memcmp(secs[i].fourcc, fc, 4) == 0) return &secs[i];
    return NULL;
}

TaudError te_parse_taud(const uint8_t *file, size_t len, TaudDoc *doc)
{
    int version, numSongs, kindBits, i, nSecs = 0, capSecs = 0;
    size_t compSize, projOff;
    Section *secs = NULL;
    const Section *sec;
    TaudError err = TAUD_OK;

    memset(doc, 0, sizeof *doc);
    if (len < TAUD_HEADER_SIZE) return TAUD_ERR_FORMAT;
    if (memcmp(file, TAUD_MAGIC, 8) != 0) return TAUD_ERR_FORMAT;
    version = file[8];
    numSongs = file[9];
    compSize = rd_u32(file + 10);
    projOff = rd_u32(file + 14);

    kindBits = version & TAUD_KIND_MASK;
    if (kindBits == TAUD_KIND_SAMPLEINST) doc->kind = 1;
    else if (kindBits == TAUD_KIND_PATTERN) doc->kind = 2;
    else if (kindBits == TAUD_KIND_FULL) doc->kind = 0;
    else return TAUD_ERR_FORMAT;
    doc->fmtVer = version & TAUD_VERSION_MASK;
    doc->wideCells = doc->fmtVer >= 3;

    /* ── Project Data sections (walked first: xHDR changes the cue stride) ── */
    if (projOff != 0 && projOff <= len && len - projOff >= 16 && memcmp(file + projOff, PROJ_MAGIC, 8) == 0) {
        size_t p = projOff + 16;
        doc->projLen = len - projOff;
        doc->projData = (uint8_t *)malloc(doc->projLen);
        if (doc->projData == NULL) return TAUD_ERR_NOMEM;
        memcpy(doc->projData, file + projOff, doc->projLen);
        while (len - p >= 8) {
            size_t secLen = rd_u32(file + p + 4);
            size_t payload = p + 8;
            if (secLen > len - payload) break;
            if (nSecs == capSecs) {
                int nc = capSecs ? capSecs * 2 : 16;
                Section *ns = (Section *)realloc(secs, sizeof *ns * (size_t)nc);
                if (ns == NULL) { free(secs); te_doc_free(doc); return TAUD_ERR_NOMEM; }
                secs = ns;
                capSecs = nc;
            }
            memcpy(secs[nSecs].fourcc, file + p, 4);
            secs[nSecs].off = payload - projOff;   /* offset into projData */
            secs[nSecs].len = secLen;
            nSecs++;
            p = payload + secLen;
        }
    }
    sec = find_sec(secs, nSecs, "xHDR");
    doc->is64Channel = (version & TAUD_XHDR_FLAG) != 0 && sec != NULL && sec->len >= 1 &&
        (doc->projData[sec->off] & 0x01) != 0;

    /* ── sample+instrument image ── */
    if (doc->kind != 2 && compSize > 0) {
        size_t outLen = 0;
        if (compSize > len - TAUD_HEADER_SIZE) { err = TAUD_ERR_FORMAT; goto fail; }
        doc->sampleInstImage = te_decompress(file + TAUD_HEADER_SIZE, compSize, SAMPLEINST_SIZE, &outLen);
        if (doc->sampleInstImage == NULL) { err = TAUD_ERR_DECOMPRESS; goto fail; }
        /* Trim the pool to its last non-zero byte and slide the instrument
         * bin down behind it; reads past the trimmed end are 0, as before. */
        {
            size_t used = SAMPLE_BIN_TOTAL;
            uint8_t *shrunk;
            while (used > 0 && doc->sampleInstImage[used - 1] == 0) used--;
            memmove(doc->sampleInstImage + used, doc->sampleInstImage + SAMPLE_BIN_TOTAL, INSTBIN_SIZE);
            shrunk = (uint8_t *)realloc(doc->sampleInstImage, used + INSTBIN_SIZE);
            if (shrunk != NULL) doc->sampleInstImage = shrunk;
            doc->samplePoolLen = used;
        }
    }

    /* ── song table + per-song pattern/cue bins ── */
    if (doc->kind != 1 && numSongs > 0) {
        size_t tableOff = TAUD_HEADER_SIZE + compSize;
        if (compSize > len - TAUD_HEADER_SIZE) { err = TAUD_ERR_FORMAT; goto fail; }
        doc->songs = (TaudSong *)calloc((size_t)numSongs, sizeof(TaudSong));
        if (doc->songs == NULL) { err = TAUD_ERR_NOMEM; goto fail; }
        doc->numSongs = numSongs;
        for (i = 0; i < numSongs; i++) {
            TaudSong *s = &doc->songs[i];
            size_t eo = tableOff + (size_t)i * TAUD_SONG_ENTRY;
            size_t songOffset, patComp, cueComp, cueLen = 0, patLen = 0;
            const uint8_t *en;
            int bpmStored, tickPacked;
            uint8_t *cueBin;
            if (eo > len || len - eo < TAUD_SONG_ENTRY) { err = TAUD_ERR_FORMAT; goto fail; }
            en = file + eo;
            songOffset = rd_u32(en);
            s->numVoices = en[4];
            s->numPats = (int)rd_u16(en + 5);
            bpmStored = en[7];
            tickPacked = en[8];
            s->tickRate = tickPacked & 0x7f;
            bpmStored |= (tickPacked & 0x80) << 1;
            s->bpm = bpmStored + 25;
            s->tuningBaseNote = (int)rd_u16(en + 9);
            s->tuningFreq = rd_f32(en + 11);
            s->globalFlags = en[15];
            s->globalVolume = en[16];
            s->mixingVolume = en[17];
            patComp = rd_u32(en + 18);
            cueComp = rd_u32(en + 22);
            s->numCuesStored = (int)rd_u16(en + 26);
            s->surroundModel = en[28] & 3;

            s->patSize = doc->fmtVer >= 3 ? PATTERN_BYTES_WIDE : PATTERN_BYTES;
            if (songOffset > len || patComp > len - songOffset || cueComp > len - songOffset - patComp) {
                err = TAUD_ERR_FORMAT;
                goto fail;
            }
            if (s->numPats > 0) {
                s->patBin = te_decompress(file + songOffset, patComp, (size_t)s->numPats * s->patSize, &patLen);
                if (s->patBin == NULL) { err = TAUD_ERR_DECOMPRESS; goto fail; }
                s->patBinLen = patLen;
            }

            cueBin = te_decompress(file + songOffset + patComp, cueComp, 0, &cueLen);
            if (cueBin == NULL) { err = TAUD_ERR_DECOMPRESS; goto fail; }
            if (doc->fmtVer >= 2) {
                size_t stride = doc->is64Channel ? CUE_SIZE_64 : CUE_SIZE;
                int chans = doc->is64Channel ? MAX_VOICES : NUM_VOICES;
                int maxCues = doc->is64Channel ? NUM_CUES_64 : NUM_CUES;
                int numCues = (int)(cueLen / stride), c, ch;
                if (numCues > maxCues) numCues = maxCues;
                s->numCues = numCues;
                s->cues = (uint16_t *)malloc(sizeof(uint16_t) * MAX_VOICES * (size_t)(numCues > 0 ? numCues : 1));
                if (s->cues == NULL) { free(cueBin); err = TAUD_ERR_NOMEM; goto fail; }
                for (c = 0; c < numCues; c++) {
                    uint16_t *w = s->cues + (size_t)c * MAX_VOICES;
                    for (ch = 0; ch < MAX_VOICES; ch++) w[ch] = CUE_EMPTY;
                    for (ch = 0; ch < chans; ch++) w[ch] = (uint16_t)rd_u16(cueBin + c * stride + ch * 2);
                }
            } else {
                int numCues = (int)(cueLen / CUE_SIZE_V1), c;
                s->numCues = numCues;
                s->cues = (uint16_t *)malloc(sizeof(uint16_t) * MAX_VOICES * (size_t)(numCues > 0 ? numCues : 1));
                if (s->cues == NULL) { free(cueBin); err = TAUD_ERR_NOMEM; goto fail; }
                for (c = 0; c < numCues; c++) v1_cue_to_words(cueBin + c * CUE_SIZE_V1, s->cues + (size_t)c * MAX_VOICES);
            }
            free(cueBin);
        }
    }

    /* ── decoded views: Ixmp, PNam, sMet, sMst ── */
    sec = find_sec(secs, nSecs, "Ixmp");
    if (sec != NULL) {
        const uint8_t *pl = doc->projData + sec->off;
        size_t q = 0, qEnd = sec->len;
        int cap = 0;
        while (q + 4 <= qEnd) {
            int instId = pl[q] | ((pl[q + 3] & 0x03) << 8);
            int patchCnt = pl[q + 1] | (pl[q + 2] << 8), k;
            size_t blobLen = 0, scan;
            bool ok = true;
            q += 4;
            scan = q;
            for (k = 0; k < patchCnt; k++) {
                int plen;
                if (scan + 31 > qEnd) { ok = false; break; }
                plen = ixmp_patch_len(pl, scan, qEnd);
                if (scan + (size_t)plen > qEnd) { ok = false; break; }
                scan += (size_t)plen;
                blobLen += (size_t)plen;
            }
            if (!ok) break;
            if (doc->numIxmp == cap) {
                int nc = cap ? cap * 2 : 16;
                TaudIxmpEntry *ne = (TaudIxmpEntry *)realloc(doc->ixmp, sizeof *ne * (size_t)nc);
                if (ne == NULL) { err = TAUD_ERR_NOMEM; goto fail; }
                doc->ixmp = ne;
                cap = nc;
            }
            doc->ixmp[doc->numIxmp].instId = instId;
            doc->ixmp[doc->numIxmp].blob = pl + q;
            doc->ixmp[doc->numIxmp].len = blobLen;
            doc->numIxmp++;
            q += blobLen;
        }
    }
    sec = find_sec(secs, nSecs, "PNam");
    if (sec != NULL) doc->projectName = str_nul(doc->projData + sec->off, 0, sec->len, NULL);
    sec = find_sec(secs, nSecs, "sMet");
    if (sec != NULL) {
        const uint8_t *pl = doc->projData + sec->off;
        size_t p = 0;
        while (sec->len - p >= 5) {
            int songIndex = pl[p];
            size_t subLen = rd_u32(pl + p + 1);
            size_t sub = p + 5;
            if (subLen > sec->len - sub) break;
            if (songIndex < doc->numSongs && subLen >= 4) {
                free(doc->songs[songIndex].name);
                doc->songs[songIndex].name = str_nul(pl, sub + 4, sub + subLen, NULL);
            }
            p = sub + subLen;
        }
    }
    sec = find_sec(secs, nSecs, "sMst");
    if (sec != NULL) {
        const uint8_t *pl = doc->projData + sec->off;
        size_t p = 0;
        while (sec->len - p >= 5) {
            int songIndex = pl[p];
            size_t subLen = rd_u32(pl + p + 1);
            size_t sub = p + 5;
            TaudMastering m;
            if (subLen > sec->len - sub) break;
            if (te_mastering_parse_block(pl + sub, subLen, &m) && songIndex < doc->numSongs) {
                doc->songs[songIndex].mastering = m;
                doc->songs[songIndex].hasMastering = true;
            }
            p = sub + subLen;
        }
    }
    free(secs);
    return TAUD_OK;

fail:
    free(secs);
    te_doc_free(doc);
    return err;
}

/* Load a parsed song into the engine — the JVM oracle's upload order. */
void te_load_into_engine(TaudEngine *e, const TaudDoc *doc, int songIndex)
{
    const TaudSong *song = &doc->songs[songIndex];
    int chans = doc->is64Channel ? MAX_VOICES : NUM_VOICES;
    uint8_t cueBytes[MAX_VOICES * 2];
    int p, c, ch;

    e->is64ChannelMode = doc->is64Channel;
    te_set_cell_format(e, doc->wideCells);
    if (doc->sampleInstImage != NULL) {
        te_upload_sample_inst_blob(e, doc->sampleInstImage, doc->samplePoolLen,
                                   doc->sampleInstImage + doc->samplePoolLen);
    }

    /* A short pattern bin uploads short patterns, as the JS subarray does. */
    for (p = 0; p < song->numPats; p++) {
        size_t off = (size_t)p * song->patSize, n = 0;
        if (off < song->patBinLen) {
            n = song->patBinLen - off;
            if (n > (size_t)song->patSize) n = (size_t)song->patSize;
        }
        te_upload_pattern(e, p, song->patBin + off, n);
    }

    for (c = 0; c < song->numCues; c++) {
        const uint16_t *words = song->cues + (size_t)c * MAX_VOICES;
        for (ch = 0; ch < chans; ch++) {
            cueBytes[ch * 2] = (uint8_t)(words[ch] & 0xff);
            cueBytes[ch * 2 + 1] = (uint8_t)((words[ch] >> 8) & 0xff);
        }
        te_upload_cue(e, c, cueBytes, (size_t)chans * 2);
    }

    e->bpm = iclamp(song->bpm, 25, 535);
    e->tickRate = (song->tickRate > 0 ? song->tickRate : 6) & 255;
    te_set_tuning(e, song->tuningBaseNote, song->tuningFreq);
    te_set_tracker_mixer_flags(e, song->globalFlags);
    te_set_surround_model(e, song->surroundModel);
    e->globalVolume = song->globalVolume & 255;
    e->mixingVolume = song->mixingVolume & 255;
    e->masterVolume = 255;
    te_set_mastering(e, song->hasMastering ? &song->mastering : NULL);

    for (p = 0; p < doc->numIxmp; p++) {
        te_upload_instrument_patches(e, doc->ixmp[p].instId, doc->ixmp[p].blob, doc->ixmp[p].len);
    }
}
