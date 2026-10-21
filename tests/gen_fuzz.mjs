// Generates a deterministic, randomised .taud that exercises as much of the
// engine as it can reach: every effect column command, both cell formats,
// 32 and 64 lanes, surround models, envelopes of every kind, IT and SF2
// filters, NNA / duplicate checks (the lane check included), Ixmp patches
// (stereo included), layered and FM metainstruments (chip operators
// included), OPL, PSG and formant instruments (sung from a sample or from a
// chip), table kinds the engine does not know, sample modifications,
// interrupts and mastering.
//
//   node tests/gen_fuzz.mjs <microtone-taudplay> SEED OUT.taud
//
// The song is not meant to sound like anything. It is meant to make the C
// port and the JavaScript library disagree if they can.
import { writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

const [root, seedArg, out] = process.argv.slice(2);
const { gzipSync } = await import(pathToFileURL(resolve(root, "core/vendor/fflate.esm.js")).href);

let seed = (+seedArg >>> 0) || 1;
const rnd = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296; };
const ri = (lo, hi) => lo + Math.floor(rnd() * (hi - lo + 1));
const pick = (a) => a[Math.floor(rnd() * a.length)];
const chance = (p) => rnd() < p;

const variant = +seedArg % 4;          // 0: v2 stereo, 1: v2 64-lane, 2: v3 planar, 3: v3 spatial
const wide = variant >= 2;
const lanes64 = variant === 1;
const surround = variant === 2 ? 1 : variant === 3 ? 2 : 0;
const fmtVer = wide ? 3 : 2;
const numLanes = lanes64 ? ri(33, 40) : ri(4, 10);

// ── sample pool ──
const SAMPLEBIN = 8388608, INSTBIN = 262144;
const image = new Uint8Array(SAMPLEBIN + INSTBIN);
const samples = [];
let poolPos = 16;
function addSample(len, fn) {
  const ptr = poolPos;
  for (let i = 0; i < len; i++) image[ptr + i] = Math.max(0, Math.min(255, Math.round(127.5 + 127.5 * fn(i, len))));
  poolPos += len + ri(0, 64);
  samples.push({ ptr, len });
  return samples.length - 1;
}
addSample(64, (i, n) => Math.sin((2 * Math.PI * i) / n));
addSample(128, (i, n) => (2 * i) / n - 1);
addSample(32, (i, n) => (i < n / 2 ? 0.8 : -0.8));
addSample(4000, (i, n) => (rnd() * 2 - 1) * Math.exp(-i / 700));
addSample(12000, (i, n) => Math.sin(i * 0.05 + Math.sin(i * 0.013) * 3) * (1 - i / n));
addSample(256, (i, n) => Math.sin((2 * Math.PI * i) / n) * 0.5 + Math.sin((6 * Math.PI * i) / n) * 0.3);
for (let k = 0; k < 4; k++) {
  const f = 0.01 + rnd() * 0.2;
  addSample(ri(200, 9000), (i) => Math.sin(i * f) * (rnd() < 0.05 ? 0.3 : 1));
}

// ── instruments ──
const rec = (idx) => image.subarray(SAMPLEBIN + idx * 256, SAMPLEBIN + (idx + 1) * 256);
const w16 = (b, o, v) => { b[o] = v & 0xff; b[o + 1] = (v >>> 8) & 0xff; };
const w32 = (b, o, v) => { w16(b, o, v & 0xffff); w16(b, o + 2, (v >>> 16) & 0xffff); };

function envWord({ present = true, loop = false, start = 0, end = 0, carry = false, bit7 = false } = {}) {
  return (present ? 1 << 13 : 0) | (loop ? 1 << 5 : 0) | ((start & 0x1f) << 8) | (end & 0x1f) |
    (carry ? 1 << 6 : 0) | (bit7 ? 1 << 7 : 0);
}
function randEnv(b, o, maxVal) {
  const n = ri(2, 24);
  for (let k = 0; k < 25; k++) {
    b[o + 2 * k] = k < n ? ri(0, maxVal) : ri(0, maxVal);
    b[o + 2 * k + 1] = k < n - 1 ? (chance(0.15) ? 0 : ri(1, 90)) : 0;
  }
  return n;
}
function randLoopWords(n, extra = {}) {
  const s = ri(0, n - 1), e = ri(s, n - 1);
  const loop = envWord({ present: true, loop: chance(0.5), start: s, end: e, ...extra });
  const s2 = ri(0, n - 1), e2 = ri(s2, n - 1);
  const sus = chance(0.5) ? (1 << 5) | (s2 << 8) | e2 : 0;
  return [loop, sus];
}

const sampleInsts = [];
for (let idx = 1; idx <= 24; idx++) {
  const b = rec(idx);
  const s = samples[ri(0, samples.length - 1)];
  w32(b, 0, s.ptr);
  w16(b, 4, s.len);
  w16(b, 6, pick([8363, 16726, 22050, 44100, ri(2000, 60000)]));
  w16(b, 8, chance(0.2) ? ri(0, s.len >> 1) : 0);
  const ls = ri(0, s.len - 2), le = ri(ls + 1, s.len);
  w16(b, 10, ls); w16(b, 12, le);
  b[14] = ri(0, 3) | (chance(0.2) ? 4 : 0) | (chance(0.1) ? 0x20 : 0);
  let n = randEnv(b, 21, 63); { const [l, su] = randLoopWords(n, { carry: chance(0.2) }); w16(b, 15, l); w16(b, 189, su); }
  if (chance(0.5)) { n = randEnv(b, 71, 255); const [l, su] = randLoopWords(n, { bit7: chance(0.5) }); w16(b, 17, l); w16(b, 191, su); }
  else w16(b, 17, chance(0.5) ? 1 << 7 : 0);
  if (chance(0.5)) { n = randEnv(b, 121, 255); const [l, su] = randLoopWords(n, { bit7: chance(0.5) }); w16(b, 19, l); w16(b, 193, su); }
  if (chance(0.3)) { n = randEnv(b, 201, 255); const [l, su] = randLoopWords(n, { bit7: chance(0.5) }); w16(b, 197, l); w16(b, 199, su); }
  b[171] = ri(128, 255);
  b[172] = ri(0, 255);
  const sf = chance(0.3);
  b[173] = ri(0, 3) | (sf ? 0x10 : 0);
  b[174] = chance(0.3) ? ri(0, 40) : 0;
  b[175] = chance(0.4) ? ri(0, 255) : 0;
  b[176] = chance(0.3) ? ri(0, 40) : 0;
  b[177] = ri(0, 255);
  w16(b, 178, ri(0x3000, 0x7000));
  b[180] = chance(0.3) ? ri(0, 255) : 0;
  b[181] = chance(0.3) ? ri(0, 60) : 0;
  b[182] = chance(0.5) ? ri(0, 255) : 0xff;
  b[183] = ri(0, 255);
  w16(b, 184, chance(0.3) ? ri(0, 0xffff) : 0);
  b[186] = ri(0, 255);
  b[187] = chance(0.4) ? ri(0, 255) : 0;
  b[188] = chance(0.3) ? ri(0, 255) : 0;
  b[195] = chance(0.3) ? ri(0, 255) : 0;
  b[196] = chance(0.5) ? ri(1, 255) : 0;
  b[251] = chance(0.3) ? ri(0, 255) : 0;
  b[252] = ri(0, 255); b[253] = ri(0, 255); b[254] = ri(0, 255);
  sampleInsts.push(idx);
}

// The ordinary fields of a typed record (OPL type 8, formant type 1): bytes
// 4…196 and 251…255, the second pitch/filter slot left to the type's block.
function typedFields(b, s) {
  w16(b, 4, s ? s.len : ri(0, 0xffff));
  w16(b, 6, chance(0.3) ? 0 : pick([8363, 16726, 22050, ri(2000, 40000)]));
  w16(b, 8, 0);
  const len = s ? s.len : 64;
  const ls = ri(0, len - 2), le = ri(ls + 1, len);
  w16(b, 10, ls); w16(b, 12, le);
  b[14] = ri(0, 3) | (chance(0.2) ? 4 : 0);
  if (chance(0.5)) { const n = randEnv(b, 21, 63); const [l, su] = randLoopWords(n, { carry: chance(0.2) }); w16(b, 15, l); w16(b, 189, su); }
  else { for (let k = 0; k < 25; k++) b[21 + 2 * k] = chance(0.5) ? 0x3f : ri(0, 63); w16(b, 15, 0); }
  if (chance(0.3)) { const n = randEnv(b, 71, 255); const [l, su] = randLoopWords(n); w16(b, 17, l); w16(b, 191, su); }
  if (chance(0.3)) { const n = randEnv(b, 121, 255); const [l, su] = randLoopWords(n, { bit7: chance(0.5) }); w16(b, 19, l); w16(b, 193, su); }
  b[171] = ri(128, 255);
  b[172] = ri(0, 255);
  b[173] = ri(0, 3) | (chance(0.3) ? 0x10 : 0);
  b[174] = chance(0.3) ? ri(0, 40) : 0;
  b[175] = chance(0.3) ? ri(0, 255) : 0;
  b[177] = ri(0, 255);
  w16(b, 178, 0x5000);
  b[182] = chance(0.4) ? ri(0, 255) : 0xff;
  b[183] = ri(0, 255);
  w16(b, 184, chance(0.3) ? ri(0, 0xffff) : 0);
  b[186] = pick([0, 1, 1, 2, 3]) | (chance(0.2) ? 0x20 : 0) | (ri(0, 7) << 2);
  b[187] = chance(0.3) ? ri(0, 255) : 0;
  b[188] = chance(0.3) ? ri(0, 255) : 0;
  b[195] = chance(0.5) ? ri(0, 255) : pick([0x04, 0x14, 0x24, 0x10, 0x02, 0x06]);
  b[196] = chance(0.5) ? ri(1, 255) : 0;
  b[251] = chance(0.3) ? ri(0, 255) : 0;
}

// OPL instruments (type 8): every kind, random registers.
const oplInsts = [];
for (let idx = 50; idx < 56; idx++) {
  const b = rec(idx);
  b.fill(0);
  b.set([0x80, 0x00, 0xff, 0xff], 0);
  typedFields(b, null);
  const blk = b.subarray(197, 251);
  blk[0] = (idx === 50 ? 0 : idx === 51 ? 1 : ri(0, 6)) | (ri(0, 15) << 3);
  blk[1] = ri(0, 15); blk[2] = ri(0, 1); blk[3] = ri(0, 15);
  w16(blk, 4, ri(-3000, 3000) & 0xffff);
  for (let k = 0; k < 4; k++) {
    const o = 6 + k * 5;
    blk[o] = ri(0, 255); blk[o + 1] = chance(0.5) ? ri(0, 63) : ri(0, 255);
    blk[o + 2] = (ri(1, 15) << 4) | ri(0, 15); blk[o + 3] = ri(0, 255); blk[o + 4] = ri(0, 7);
  }
  oplInsts.push(idx);
}

// PSG instruments (type 9): two oscillators, every waveform, both mixes,
// mutes, the chip envelope, tremolo/vibrato, X resets and the auto-advance.
const psgInsts = [];
for (let idx = 70; idx < 76; idx++) {
  const b = rec(idx);
  b.fill(0);
  b.set([0x90, 0x00, 0xff, 0xff], 0);
  typedFields(b, null);
  const blk = b.subarray(197, 251);
  blk[0] = (chance(0.5) ? 1 : 0) | (chance(0.15) ? 4 : 0) | (chance(0.15) ? 8 : 0) | (chance(0.1) ? ri(0, 255) & 0xf2 : 0);
  blk[1] = chance(0.2) ? ri(0, 255) : 0;
  for (let k = 0; k < 2; k++) {
    const o = 2 + k * 12;
    const wave = idx === 70 + k ? ri(1, 7) : ri(0, 15);
    blk[o] = (wave << 4) | ri(1, 15);
    blk[o + 1] = (ri(0, 15) << 4) | ri(0, 15);
    blk[o + 2] = (ri(1, 15) << 4) | ri(0, 15);
    blk[o + 3] = (ri(0, 3) << 6) | (chance(0.6) ? ri(0, 20) : ri(0, 63));
    blk[o + 4] = ri(0, 255);
    blk[o + 5] = chance(0.4) ? 0 : pick([1, 2, 4, 8, ri(1, 255)]);
    blk[o + 6] = ri(0, 3) | (chance(0.1) ? ri(0, 255) & 0xfc : 0);
    if (chance(0.1)) for (let j = 7; j < 12; j++) blk[o + j] = ri(0, 255);
  }
  psgInsts.push(idx);
}

// Formant instruments (type 1): a source, a vowel, consonant slots, a glide.
const CONS_IDS = [0, 0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 13, 14, 15, 16, 32, 34, 37, 40, 41, 44, 45, 48, 49, 50, 51,
  52, 54, 55, 56, 58, 60, 62, 64, 66, 67, 68, 69, 70, 72, 74, 80, 81, 83, 85, 86, 88, 90, 96, 98, 100, 104, 105, 200];
const formantInsts = [];
for (let idx = 60; idx < 68; idx++) {
  const b = rec(idx);
  b.fill(0);
  b.set([0x10, 0x00, 0xff, 0xff], 0);
  // The source instrument (bits 8…9 in byte 0, 0…7 in byte 1): a chip one
  // sings from it; anything else (itself, a sample, a formant) is ignored.
  if (idx >= 64 || chance(0.25)) {
    const src = idx >= 66 ? pick(psgInsts) : idx >= 64 ? pick(oplInsts)
      : pick([...oplInsts, ...psgInsts, idx, 1, 61, 999]);
    b[0] |= (src >>> 8) & 3;
    b[1] = src & 0xff;
  }
  const s = samples[pick([0, 1, 2, 5, ri(0, samples.length - 1)])];
  typedFields(b, s);
  const blk = b.subarray(197, 251);
  blk[0] = s.ptr & 0xff; blk[1] = (s.ptr >>> 8) & 0xff; blk[2] = (s.ptr >>> 16) & 0xff;
  blk[3] = ri(0, 255); blk[4] = ri(0, 255); blk[5] = ri(0, 255);
  blk[6] = pick([0, 64, ri(0, 255)]);
  // The reference note (0 = no vowel: a syllabic consonant) and bit 15, a
  // nasalised vowel.
  w16(blk, 7, pick([0x4000, 0x5000, ri(0x3000, 0x7000), 0, 0]) | (chance(0.3) ? 0x8000 : 0));
  blk[9] = pick([0, 20, ri(0, 255)]);
  blk[10] = pick([128, ri(64, 192), ri(0, 255)]);
  const lo = pick([s.ptr, 16]);
  w16(blk, 11, lo & 0xffff); blk[13] = lo >>> 16;
  w16(blk, 14, (lo + 4096) & 0xffff); blk[16] = (lo + 4096) >>> 16;
  for (let k = 0; k < 3; k++) {
    const o = 17 + k * 12;
    const ns = samples[ri(0, samples.length - 1)];
    blk[o] = pick(CONS_IDS);
    if (chance(0.6)) {
      blk[o + 1] = ns.ptr & 0xff; blk[o + 2] = (ns.ptr >>> 8) & 0xff; blk[o + 3] = ns.ptr >>> 16;
      w16(blk, o + 4, ns.len); w16(blk, o + 6, chance(0.5) ? 0 : ri(0, ns.len));
      w16(blk, o + 8, chance(0.1) ? 0 : ri(2000, 48000));
    }
    blk[o + 10] = chance(0.5) ? 0 : ri(0, 255);
    blk[o + 11] = chance(0.5) ? 0 : ri(0, 255);
  }
  // CCGVRC's G (bits 0–2), the onset's secondary articulation (3–4), the
  // coda's release (5–6) and R (bit 7).
  blk[53] = (chance(0.5) ? ri(0, 7) : 0) | (ri(0, 3) << 3) | (ri(0, 3) << 5) | (chance(0.3) ? 0x80 : 0);
  // Now and then nothing to say at all: no vowel and no consonant.
  if (chance(0.08)) { w16(blk, 7, 0); blk[17] = 0; blk[29] = 0; blk[41] = 0; }
  formantInsts.push(idx);
}

// Layered metainstruments (type 0) and FM racks (type 4).
function metaRecord(idx, type, layers, program, strict) {
  const b = rec(idx);
  b.fill(0);
  b[0] = ((type & 0xf) << 4) | (strict ? 1 : 0);
  b[1] = layers.length;
  b[2] = 0xff; b[3] = 0xff;
  let o = 4;
  for (const l of layers) {
    b[o] = l.inst & 0xff;
    b[o + 1] = l.mix;
    w16(b, o + 2, l.detune & 0xffff);
    w16(b, o + 4, l.ps); w16(b, o + 6, l.pe);
    b[o + 8] = (l.vs & 0x3f) | (((l.inst >>> 8) & 3) << 6);
    b[o + 9] = (l.ve & 0x3f) | (l.fixed ? 0x40 : 0);
    o += 10;
  }
  if (program) {
    for (const w of program) { w16(b, o, w); o += 2; }
    w16(b, o, 0xffff);
  }
}
for (let idx = 30; idx < 34; idx++) {
  const nL = ri(1, 4);
  const layers = [];
  for (let k = 0; k < nL; k++) {
    const ps = chance(0.7) ? 0 : ri(0, 0x8000);
    layers.push({ inst: chance(0.75) ? pick(sampleInsts) : pick([...oplInsts, ...psgInsts, ...formantInsts]), mix: ri(100, 200), detune: ri(-2000, 2000),
      ps, pe: chance(0.7) ? 0xffff : ri(ps, 0xffff), vs: 0, ve: chance(0.8) ? 63 : ri(0, 63),
      fixed: chance(0.2) });
  }
  if (layers[0].fixed) layers[0].detune = ri(0x3000, 0x7000);
  for (const l of layers) if (l.fixed) l.detune = ri(0x3000, 0x7000);
  metaRecord(idx, 0, layers, null, chance(0.2));
}
for (let idx = 40; idx < 43; idx++) {
  const nOps = ri(2, 4);
  const ops = [];
  for (let k = 0; k < nOps; k++) {
    // Chip operators (OPL, PSG) are read with phase modulation; a formant
    // instrument is a high meta and is refused.
    ops.push({ inst: chance(0.7) ? pick(sampleInsts.slice(0, 6)) : pick([...oplInsts, ...psgInsts, ...formantInsts]), mix: ri(120, 200), detune: ri(-300, 3000),
      ps: 0, pe: 0xffff, vs: 0, ve: 63, fixed: false });
  }
  const progs = [
    [nOps - 1, 0x0400 | 0],
    [0x0800 | 0, 0x0400 | 0],
    [1, 0x0400 | 0, 0, 0xff00],
    [1, 0xff03, 0x0400 | 0, 0xff01],
    [nOps - 1, 0xff02, 0x0400 | 0],
  ];
  metaRecord(idx, 4, ops, pick(progs).map((w) => (w & 0xff00) === 0xff00 ? w : (w & ~0x3ff) | Math.min(w & 0x3ff, nOps - 1)), false);
}
// Tables of kinds the engine has no reading for (types 2, 3, 5, 7, 10…15):
// the Layered shape, sounding nothing.
for (let idx = 34; idx < 36; idx++) {
  metaRecord(idx, pick([2, 3, 5, 7, 10, 11, 12, 13, 14, 15]),
    [{ inst: pick(sampleInsts), mix: 160, detune: 0, ps: 0, pe: 0xffff, vs: 0, ve: 63, fixed: false }], null, false);
}

// ── Ixmp patches ──
function patchRecord(p) {
  const bytes = [];
  let ver = 0x01;
  if (p.extra) ver |= 0x80;
  if (p.vol) ver |= 0x02;
  if (p.chan) ver |= 0x20;
  const b8 = (v) => bytes.push(v & 0xff);
  const b16 = (v) => { b8(v); b8(v >>> 8); };
  const b32 = (v) => { b16(v & 0xffff); b16(v >>> 16); };
  b8(ver); b16(p.ps); b16(p.pe); b8(p.vs); b8(p.ve);
  b32(p.ptr); b16(p.len); b16(p.play); b16(p.ls); b16(p.le); b16(p.rate); b16(p.detune & 0xffff);
  b8(p.loop); b8(p.pan); b8(p.dnv); b8(p.vs1); b8(p.vs2); b8(p.vd); b8(p.vr); b8(p.vw);
  if (p.extra) { b32(p.sf ? 1 : 0); b32(0); b16(p.fade); b16(p.cut); b16(p.res); b8(p.atten); }
  if (p.vol) { b16(envWord({ loop: true, start: 1, end: 3 })); b16(0); for (let k = 0; k < 25; k++) { b8(ri(0, 63)); b8(k < 5 ? ri(1, 60) : 0); } }
  if (p.chan) { b8((1 << 4) | p.chanMode); b8(0); b8(0); b8(0); b32(p.ptr2); }
  return bytes;
}
const ixmp = [];
for (const idx of [...sampleInsts.slice(0, 10), oplInsts[0], psgInsts[0], formantInsts[0]]) {
  if (!chance(0.6)) continue;
  const n = ri(1, 3);
  const bytes = [];
  for (let k = 0; k < n; k++) {
    const s = samples[ri(0, samples.length - 1)], s2 = samples[ri(0, samples.length - 1)];
    const len = Math.min(s.len, s2.len);
    const ls = ri(0, len - 2);
    bytes.push(...patchRecord({
      ps: k === n - 1 ? 0 : ri(0, 0x6000), pe: 0xffff, vs: 0, ve: 63,
      ptr: s.ptr, len, play: 0, ls, le: ri(ls + 1, len), rate: pick([8363, 22050, 44100]),
      detune: ri(-500, 500), loop: ri(0, 3), pan: chance(0.5) ? 0xff : ri(0, 254), dnv: chance(0.5) ? 0 : ri(1, 255),
      vs1: 0, vs2: 0, vd: 0, vr: 0, vw: 0xff,
      extra: chance(0.5), sf: chance(0.5), fade: ri(0, 200), cut: ri(0, 0xffff), res: ri(0, 0x3ff), atten: ri(0, 255),
      vol: chance(0.3), chan: chance(0.4), chanMode: ri(0, 1), ptr2: s2.ptr,
    }));
  }
  ixmp.push({ inst: idx, count: n, bytes });
}

// ── patterns ──
const EFFECTS = [
  [0x01, () => (pick([0, 1, 2, 4, 8, 12, 16, 20, 5, 9]) << 8)],
  [0x02, () => ri(0, 0xffff)], [0x03, () => ri(0, 0xffff)],
  [0x05, () => chance(0.2) ? 0xffff : ri(0, 0xffff)], [0x06, () => chance(0.2) ? 0xffff : ri(0, 0xffff)],
  [0x07, () => (ri(1, 4) << 8) | ri(1, 4)],
  [0x08, () => ri(0, 0xffff)], [0x09, () => (ri(0, 3) << 12) | ri(0, 255)],
  [0x0a, () => ri(1, 12) << 8],
  [0x0b, () => ri(0, 1)], [0x0c, () => ri(0, 63)],
  [0x0d, () => ri(0, 255) << 8], [0x0e, () => chance(0.3) ? 0xf000 | ri(0, 0xfff) : ri(0, 0x200)],
  [0x0f, () => chance(0.3) ? 0xf000 | ri(0, 0xfff) : ri(0, 0x200)],
  [0x10, () => ri(0, 0x400)], [0x11, () => ri(0, 0xffff)], [0x12, () => ri(0, 0x0f0f)],
  [0x13, () => ri(0, 0xffff)], [0x14, () => ri(0, 255) << 8], [0x15, () => ri(0, 255) << 8],
  [0x16, () => ri(0, 70) << 8], [0x17, () => ri(0, 255) << 8],
  [0x18, () => ri(0, 0x2000)], [0x19, () => ri(0, 255) << 8],
  [0x1a, () => ri(0, 255) << 8], [0x1b, () => ri(0, 0xffff)],
  [0x1c, () => {
    const sub = pick([0, 1, 2, 3, 4, 5, 6, 7, 7, 8, 0xa, 0xa, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf]);
    // S $A on a PSG voice: an auto-advance, a level, the mutes.
    if (sub === 0xa && chance(0.5)) {
      return pick([0xa000 | ri(0, 0xff), 0xa100 | ri(0, 0x3f), 0xa800 | ri(0, 0xff), 0xa900 | ri(0, 0x3f),
        0xaf00 | ri(0, 3), 0xaf00 | ri(0, 0xff), 0xa200 | ri(0, 0xff)]);
    }
    if (sub === 0xd) return 0xd000 | (ri(0, 5) << 8) | (ri(0, 4) << 4) | ri(0, 4);
    if (sub === 0xb) return 0xb000 | (ri(0, 2) << 8);
    if (sub === 0xe) return 0xe000 | (ri(0, 2) << 8);
    return (sub << 12) | ri(0, 0xfff);
  }],
  [0x1d, () => pick([ri(0x20, 0xff) << 8, 0xff00 | ri(0, 255), ri(0, 0x1f), (ri(0xfb, 0xfe) << 8) | ri(0, 255)])],
  [0x1e, () => ri(0, 0xffff)], [0x1f, () => ri(0, 255) << 8],
  [0x20, () => ri(0, 255) << 8], [0x21, () => ri(0, 0xffff)], [0x22, () => ri(0, 0xffff)],
  [0x23, () => chance(0.5) ? 0xf000 | ri(0, 0xfff) : ri(0, 0xfff)],
  [0x04, () => ri(0, 0xffff)],
];
const ROWS = 64;
const cellBytes = wide ? 16 : 8;
function randNote() {
  const r = rnd();
  if (r < 0.04) return 1;
  if (r < 0.06) return 2;
  if (r < 0.07) return 3;
  if (r < 0.08) return 4;
  if (r < 0.10) return ri(0x10, 0x1f);
  return ri(0x3000, 0x7000);
}
// FUZZ_WILD=1 overwrites every OPL, PSG and formant block with random bytes
// (for the sanitizer build: pointers, lengths and rates anywhere at all).
if (process.env.FUZZ_WILD) {
  for (const idx of [...oplInsts, ...psgInsts, ...formantInsts]) {
    const blk = rec(idx).subarray(197, 251);
    for (let k = 0; k < blk.length; k++) blk[k] = ri(0, 255);
    if (formantInsts.includes(idx)) for (let k = 4; k < 15; k++) rec(idx)[k] = ri(0, 255);
  }
}

// FUZZ_INSTS=opl|psg|formant|fm narrows the patterns to one kind, to isolate
// a failure.
const instChoices = process.env.FUZZ_INSTS === "opl" ? oplInsts
  : process.env.FUZZ_INSTS === "psg" ? psgInsts
  : process.env.FUZZ_INSTS === "formant" ? formantInsts
  : process.env.FUZZ_INSTS === "fm" ? [40, 41, 42]
  : [...sampleInsts, 30, 31, 32, 33, 34, 35, 40, 41, 42, ...oplInsts, ...oplInsts, ...psgInsts, ...psgInsts,
    ...formantInsts, ...formantInsts];
function makePattern() {
  const p = new Uint8Array(ROWS * cellBytes);
  for (let r = 0; r < ROWS; r++) {
    const o = r * cellBytes;
    const busy = chance(0.35);
    const note = busy ? randNote() : (chance(0.1) ? 0 : 0);
    const inst = busy && chance(0.8) ? pick(instChoices) : (chance(0.05) ? pick(instChoices) : 0);
    w16(p, o, note);
    p[o + 2] = inst;
    const volEff = chance(0.6) ? 0 : ri(1, 3);
    const hasFx = chance(0.45);
    const [op, argf] = hasFx ? pick(EFFECTS) : [0, () => 0];
    const arg = hasFx ? argf() : 0;
    if (!wide) {
      p[o + 3] = (chance(0.5) ? ri(0, 63) : 0) | ((chance(0.5) ? 3 : volEff) << 6);
      p[o + 4] = (chance(0.4) ? ri(0, 63) : 0) | ((chance(0.6) ? 3 : ri(0, 2)) << 6);
      p[o + 5] = op;
      w16(p, o + 6, arg);
    } else {
      p[o + 3] = chance(0.5) ? ri(0, 255) : 0;
      const az = ri(0, 511);
      p[o + 4] = az & 0xff;
      p[o + 5] = op;
      w16(p, o + 6, arg);
      const panEff = chance(0.6) ? 3 : ri(0, 2);
      p[o + 8] = ((az >>> 8) << 7) | ((chance(0.5) ? 3 : volEff) << 4) | panEff;
      p[o + 9] = ri(0, 255);
      if (chance(0.3)) {
        if (chance(0.4)) { p[o + 10] = 0xba; w16(p, o + 11, ri(0, 0xffff)); }
        else { const [op2, a2] = pick(EFFECTS); p[o + 10] = op2; w16(p, o + 11, a2()); }
      }
    }
  }
  return p;
}

const songs = [];
const nSongs = ri(1, 2);
for (let s = 0; s < nSongs; s++) {
  const nPats = ri(3, 8);
  const pats = [];
  for (let k = 0; k < nPats; k++) pats.push(makePattern());
  const nCues = ri(3, 6);
  const chans = lanes64 ? 64 : 32;
  const stride = chans * 2;
  const cues = new Uint8Array(nCues * stride);
  for (let c = 0; c < nCues; c++) {
    const words = new Array(chans).fill(0x7fff);
    for (let ch = 0; ch < numLanes; ch++) words[ch] = chance(0.9) ? ri(0, nPats - 1) : 0x7fff;
    let w0 = 0;
    if (c === nCues - 1) w0 = 0x0100;                       // HALT
    else if (chance(0.15)) w0 = 0x0200 | ri(8, 40);          // pattern length
    else if (chance(0.03)) w0 = 0x0100 | 0x40 | ri(8, 50);   // halt at x
    for (let ch = 0; ch < 16; ch++) if ((w0 >> ch) & 1) words[ch] |= 0x8000;
    for (let ch = 0; ch < chans; ch++) w16(cues, c * stride + ch * 2, words[ch]);
  }
  songs.push({ pats, cues, nPats, nCues, bpm: chance(0.25) ? ri(537, 1048) : ri(80, 200), tick: ri(3, 8),
    flags: pick([0, 0, 1, 2, 4, 8, 12, 16, 20]), gv: ri(64, 255), mv: ri(64, 255),
    tuneNote: pick([0, 0xa000, 0x5000]), tuneFreq: pick([0, 8363, 261.6255653005986, 440]) });
}

// ── assemble ──
const LEVEL = process.env.TAUD_GZIP_LEVEL === undefined ? 6 : +process.env.TAUD_GZIP_LEVEL;
const imageComp = gzipSync(image, { mtime: 0, level: LEVEL });
const header = new Uint8Array(32);
header.set([0x1f, 0x54, 0x53, 0x56, 0x4d, 0x61, 0x75, 0x64]);
header[8] = fmtVer | (lanes64 ? 0x20 : 0);
header[9] = songs.length;
w32(header, 10, imageComp.length);
const sig = "taudplay-fuzz ";
for (let i = 0; i < 14; i++) header[18 + i] = sig.charCodeAt(i);

const tableOff = 32 + imageComp.length;
const table = new Uint8Array(songs.length * 32);
const songBlobs = [];
let off = tableOff + table.length;
songs.forEach((s, i) => {
  const patBin = new Uint8Array(s.nPats * ROWS * cellBytes);
  s.pats.forEach((p, k) => patBin.set(p, k * ROWS * cellBytes));
  const pc = gzipSync(patBin, { mtime: 0, level: LEVEL });
  const cc = gzipSync(s.cues, { mtime: 0, level: LEVEL });
  const e = i * 32;
  w32(table, e, off);
  table[e + 4] = numLanes;
  w16(table, e + 5, s.nPats);
  const bpmStored = s.bpm - 25;
  table[e + 7] = bpmStored & 0xff;
  table[e + 8] = s.tick | ((bpmStored >> 8) & 1) << 7;
  w16(table, e + 9, s.tuneNote);
  new DataView(table.buffer).setFloat32(e + 11, s.tuneFreq, true);
  table[e + 15] = s.flags;
  table[e + 16] = s.gv;
  table[e + 17] = s.mv;
  w32(table, e + 18, pc.length);
  w32(table, e + 22, cc.length);
  w16(table, e + 26, s.nCues);
  table[e + 28] = surround | ((bpmStored >> 9) & 1) << 7;
  songBlobs.push(pc, cc);
  off += pc.length + cc.length;
});

// Project data: PNam, xHDR, Ixmp, sMst.
const sections = [];
const sec = (fc, payload) => {
  const h = new Uint8Array(8);
  for (let i = 0; i < 4; i++) h[i] = fc.charCodeAt(i);
  w32(h, 4, payload.length);
  sections.push(h, payload);
};
sec("PNam", new TextEncoder().encode(`fuzz ${seedArg}\0`));
if (lanes64) sec("xHDR", Uint8Array.from([1]));
if (ixmp.length) {
  const bytes = [];
  for (const e of ixmp) { bytes.push(e.inst & 0xff, e.count & 0xff, (e.count >>> 8) & 0xff, (e.inst >>> 8) & 3, ...e.bytes); }
  sec("Ixmp", Uint8Array.from(bytes));
}
if (chance(0.6)) {
  const blk = new Uint8Array(160);
  const dv = new DataView(blk.buffer);
  const f = (o, v) => dv.setFloat32(o, v, true);
  blk[0] = 1; blk[1] = 1;
  f(4, (rnd() - 0.5) * 12);
  blk[8] = (chance(0.5) ? 1 : 0) | (chance(0.5) ? 2 : 0); f(12, ri(10, 300));
  blk[16] = chance(0.6) ? 1 : 0;
  for (let b = 0; b < 4; b++) {
    const o = 20 + b * 16;
    blk[o] = chance(0.7) ? 1 : 0; blk[o + 1] = ri(0, 2);
    f(o + 4, ri(40, 12000)); f(o + 8, (rnd() - 0.5) * 24); f(o + 12, 0.3 + rnd() * 3);
  }
  blk[84] = (chance(0.6) ? 1 : 0) | (chance(0.5) ? 2 : 0);
  f(88, -ri(3, 30)); f(92, 1 + rnd() * 6); f(96, 1 + rnd() * 40); f(100, 20 + rnd() * 400); f(104, rnd() * 10); f(108, rnd() * 6);
  blk[112] = chance(0.5) ? 1 : 0; f(116, rnd() * 2);
  blk[120] = (chance(0.6) ? 1 : 0) | (chance(0.5) ? 2 : 0); f(124, -rnd() * 6); f(128, 10 + rnd() * 200);
  f(132, (rnd() - 0.5) * 6);
  const payload = new Uint8Array(5 + 160);
  payload[0] = 0; w32(payload, 1, 160); payload.set(blk, 5);
  sec("sMst", payload);
}
const projHeader = new Uint8Array(16);
projHeader.set([0x1e, 0x54, 0x61, 0x75, 0x64, 0x50, 0x72, 0x4a]);
const projOff = off;
w32(header, 14, projOff);

const parts = [header, imageComp, table, ...songBlobs, projHeader, ...sections];
const total = parts.reduce((n, p) => n + p.length, 0);
const file = new Uint8Array(total);
let o = 0;
for (const p of parts) { file.set(p, o); o += p.length; }
writeFileSync(out, file);
console.error(`fuzz ${seedArg}: v${fmtVer} ${lanes64 ? "64-lane" : "32-lane"} surround=${surround} lanes=${numLanes} songs=${songs.length} ${file.length} bytes`);
