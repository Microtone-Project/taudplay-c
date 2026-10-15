// Emits (function id, args, V8 result) records for tests/jsmath_check.c.
//   node tests/jsmath_ref.mjs COUNT > tests/out/jsmath.bin
const N = +(process.argv[2] ?? 1000000);
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296; };
const u = (a, b) => a + (b - a) * rnd();
const gens = [
  ["sin", () => [rnd() < 0.5 ? u(-10, 10) : u(-1e5, 1e5)], (a) => Math.sin(a)],
  ["cos", () => [rnd() < 0.5 ? u(-10, 10) : u(-1e5, 1e5)], (a) => Math.cos(a)],
  ["tan", () => [rnd() < 0.5 ? u(-2, 2) : u(-1e4, 1e4)], (a) => Math.tan(a)],
  ["exp", () => [rnd() < 0.5 ? u(-30, 30) : u(-745, 709)], (a) => Math.exp(a)],
  ["log10", () => [rnd() < 0.5 ? u(0, 4) : Math.exp(u(-700, 700))], (a) => Math.log10(a)],
  ["log2", () => [rnd() < 0.5 ? u(0, 4) : Math.exp(u(-700, 700))], (a) => Math.log2(a)],
  ["acos", () => [u(-1, 1)], (a) => Math.acos(a)],
  ["asin", () => [u(-1, 1)], (a) => Math.asin(a)],
  ["atan2", () => [u(-10, 10), rnd() < 0.1 ? u(-1e-3, 1e-3) : u(-10, 10)], (a, b) => Math.atan2(a, b)],
  ["log", () => [rnd() < 0.5 ? u(0, 4) : Math.exp(u(-700, 700))], (a) => Math.log(a)],
  ["pow", () => {
    const r = rnd();
    if (r < 0.3) return [2, Math.round(u(-90000, 90000)) / 4096];
    if (r < 0.5) return [10, u(-100, 100) / 20];
    if (r < 0.7) return [u(0, 1000), u(-50, 50)];
    if (r < 0.8) return [-u(0, 10), Math.round(u(-20, 20))];
    return [Math.exp(u(-300, 300)), u(-2, 2)];
  }, (a, b) => a ** b],
];
const rec = new Float64Array(4);
const buf = Buffer.alloc(N * gens.length * 32);
let o = 0;
for (let i = 0; i < N; i++) {
  gens.forEach(([name, g, f], id) => {
    const args = g();
    rec[0] = id; rec[1] = args[0]; rec[2] = args[1] ?? 0; rec[3] = f(...args);
    Buffer.from(rec.buffer).copy(buf, o); o += 32;
  });
}
process.stdout.write(buf);
