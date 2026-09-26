#!/bin/sh
# API scenario: JS TaudRenderer vs the C API, same calls in the same order.
#   sh tests/scenario.sh FILE [RATE] [BINAURAL]
JS=${JS:-../microtone-taudplay}
OUT=tests/out/scen
mkdir -p "$OUT"
f=$1; rate=${2:-48000}; bin=${3:-0}
node tests/scenario_ref.mjs "$JS" "$f" "$rate" "$bin" "$OUT/js" || exit 1
./tests/scenario "$f" "$rate" "$bin" "$OUT/c" 0 || exit 1
cp "$OUT/c.log" "$OUT/c0.log"
./tests/scenario "$f" "$rate" "$bin" "$OUT/c" 1 || exit 1
status=0
printf 'float:'; ./tests/compare "$OUT/js.f32" "$OUT/c.f32" || status=1
if cmp -s "$OUT/js.u8" "$OUT/c.u8"; then echo "u8: identical ($(wc -c < "$OUT/c.u8") bytes)"; else echo "u8: DIFFER"; status=1; fi
if cmp -s "$OUT/js.log" "$OUT/c0.log" && cmp -s "$OUT/js.log" "$OUT/c.log"; then echo "log: identical ($(wc -l < "$OUT/c.log") lines, $(grep -c '^int' "$OUT/c.log") interrupts)"; else echo "log: DIFFER"; diff "$OUT/js.log" "$OUT/c0.log" | head -5; status=1; fi
exit $status
