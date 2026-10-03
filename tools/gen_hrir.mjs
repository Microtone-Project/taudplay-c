// Regenerates src/hrir_sadie.c from the JavaScript library's HRIR table.
//   node tools/gen_hrir.mjs ../microtone-taudplay > src/hrir_sadie.c
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

const root = resolve(process.argv[2] ?? "../microtone-taudplay");
const m = await import(pathToFileURL(resolve(root, "core/engine/hrir-sadie.js")).href);
const f = m.decodeShHrir();
const lines = [];
lines.push("/* GENERATED FILE — do not edit. Rebuild with: node tools/gen_hrir.mjs <microtone-taudplay>");
lines.push(" *");
lines.push(" * GoogleVR / SADIE spherical-harmonic HRIR set, order 3 (16 ambisonic");
lines.push(" * channels), 256 taps at 48000 Hz, as taken from Google Omnitone");
lines.push(" * (src/resources/sh_hrir_order_3.wav). Channel k is the LEFT ear's impulse");
lines.push(" * response for ambisonic channel k in ACN order, SN3D normalised; stored");
lines.push(" * channel-major as int16 (value / 32768 = tap).");
lines.push(" *");
lines.push(" * Copyright (c) 2017 Google Inc. and (c) 2017 University of York, licensed");
lines.push(" * under the Apache License 2.0. The measurements are the SADIE project's");
lines.push(" * Google/VR binaural filter set:");
lines.push(" * https://www.york.ac.uk/sadie-project/GoogleVRSADIE.html");
lines.push(" */");
lines.push("");
lines.push('#include "taud_internal.h"');
lines.push("");
lines.push(`const int16_t HRIR_SADIE[${m.HRIR_CHANNELS} * ${m.HRIR_LENGTH}] = {`);
for (let i = 0; i < f.length; i += 16) {
  const row = [];
  for (let k = i; k < Math.min(i + 16, f.length); k++) row.push(Math.round(f[k] * 32768));
  lines.push("  " + row.join(", ") + ",");
}
lines.push("};");
process.stdout.write(lines.join("\n") + "\n");
