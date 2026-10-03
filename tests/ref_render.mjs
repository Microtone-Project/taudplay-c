// Renders a song through the JavaScript taudplay library with the engine's
// random source seeded exactly as the C default (mulberry32, seed 1), and
// writes interleaved stereo float32 at the engine rate.
//
//   node tests/ref_render.mjs <microtone-taudplay> FILE.taud SONG SECONDS OUT.f32 [binaural]
import { readFileSync, writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

const [root, file, song, seconds, out, mode] = process.argv.slice(2);
const lib = (p) => import(pathToFileURL(resolve(root, p)).href);
const { TaudRenderer } = await lib("src/taudplay/render.js");
const { setRandomSource, makeSeededRandom } = await lib("core/engine/rng.js");

setRandomSource(makeSeededRandom(1));
const r = new TaudRenderer(readFileSync(file), { songIndex: +song, binaural: mode === "binaural" });
r.play();
const pcm = r.render(+seconds);
writeFileSync(out, Buffer.from(pcm.buffer, pcm.byteOffset, pcm.byteLength));
console.error(`ref: ${file} song ${song}: ${(pcm.length / 2 / 48000).toFixed(2)} s`);
