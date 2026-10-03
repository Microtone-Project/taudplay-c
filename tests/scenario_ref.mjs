// The API scenario of tests/scenario.c, driven through the JavaScript
// TaudRenderer: same calls, same order. Writes the float stream, the U8
// stream and a log of every probe / gain / interrupt reading as hex bits.
//   node tests/scenario_ref.mjs <microtone-taudplay> FILE RATE BINAURAL OUTPREFIX
import { readFileSync, writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

const [root, file, rate, binaural, prefix] = process.argv.slice(2);
const lib = (p) => import(pathToFileURL(resolve(root, p)).href);
const { setSamplingRate } = await lib("core/engine/constants.js");
setSamplingRate(+rate);
const { TaudRenderer } = await lib("src/taudplay/render.js");
const { setRandomSource, makeSeededRandom } = await lib("core/engine/rng.js");
setRandomSource(makeSeededRandom(1));

const hex = (x) => { const b = Buffer.alloc(8); b.writeDoubleLE(x); return b.readBigUInt64LE().toString(16).padStart(16, "0"); };
const log = [];
const f32 = [];
const u8 = [];
const r = new TaudRenderer(readFileSync(file), { binaural: binaural === "1" });
let block = 0;
for (let n = 0; n < 16; n++) r.setInterrupt(n, (arg) => log.push(`int ${n} ${arg} @${block}`));
const probes = (tag) => {
  const parts = [tag, r.playing ? 1 : 0, r.cue, r.row, r.bpm, r.speed, r.channelCount];
  for (let v = 0; v < 64; v++) parts.push(hex(r.getVoiceVolume(v)), hex(r.getVoicePan(v)), hex(r.getVoiceGain(v)));
  log.push(parts.join(" "));
};
const render = (n) => {
  for (let i = 0; i < n; i++) {
    const b = r.renderChunk();
    block++;
    if (b === null) { log.push(`null @${block}`); return; }
    f32.push(Float32Array.from(b));
    u8.push(Uint8Array.from(r._u8));
  }
};
r.play();
render(100); probes("a");
r.setVoiceGain(1, 0.25, 0.5);
r.setVoiceGain(3, 0.0, 0);
render(150); probes("b");
r.setVoiceGain(1, 1.0, 1.0);
r.setVolume(0.7);
render(150); probes("c");
r.seekCue(1);
render(200); probes("d");
if (r.songs.length > 1) { r.selectSong(1); r.play(); render(200); probes("e"); r.selectSong(0); }
r.stop(); render(3); probes("f");
r.play(); render(300); probes("g");
r.setVoiceGain(0, 0.0, 2.0);
render(3000); probes("h");

const cat = (arrs, T) => { const n = arrs.reduce((a, x) => a + x.length, 0); const o = new T(n); let p = 0; for (const x of arrs) { o.set(x, p); p += x.length; } return o; };
const F = cat(f32, Float32Array), U = cat(u8, Uint8Array);
writeFileSync(prefix + ".f32", Buffer.from(F.buffer));
writeFileSync(prefix + ".u8", Buffer.from(U.buffer));
writeFileSync(prefix + ".log", log.join("\n") + "\n");
