// The WAV encoder of tests/wavcheck.c, run through the JavaScript library:
// a fixed float signal (written out too, so the C side encodes the very same
// floats) encoded clean and gritty at the engine rate and at 44.1 kHz.
//   node tests/wav_ref.mjs <microtone-taudplay> OUTPREFIX
import { writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

const [root, prefix] = process.argv.slice(2);
const lib = (p) => import(pathToFileURL(resolve(root, p)).href);
const { encodeWav } = await lib("src/taudplay/render.js");
const { SAMPLING_RATE } = await lib("core/engine/constants.js");

// Two seconds: a quiet fade (where the dither earns its keep), a loud chord
// pushed past full scale (the clamps), and silence.
const frames = SAMPLING_RATE * 2;
const f32 = new Float32Array(frames * 2);
for (let n = 0; n < frames; n++) {
  const t = n / SAMPLING_RATE;
  const fade = n < frames / 2 ? 0.002 * (1 - (2 * n) / frames) : 0;
  const loud = n >= frames / 2 && n < (frames * 3) / 4 ? 1.3 : 0;
  f32[n * 2] = fade * Math.sin(2 * Math.PI * 440 * t) + loud * Math.sin(2 * Math.PI * 110 * t);
  f32[n * 2 + 1] = fade * Math.sin(2 * Math.PI * 660 * t) - loud * Math.sin(2 * Math.PI * 165 * t);
}
writeFileSync(`${prefix}.f32`, Buffer.from(f32.buffer));
for (const rate of [SAMPLING_RATE, 44100]) {
  for (const q of ["clean", "gritty"]) writeFileSync(`${prefix}.${q}.${rate}.wav`, encodeWav(f32, rate, q));
}
