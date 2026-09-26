#!/bin/sh
# Differential fuzzing: generate a random song per seed, render it through the
# JS library and the C port, and require bit-identical output.
#   sh tests/fuzz.sh FIRST_SEED LAST_SEED [SECONDS]
JS=${JS:-../microtone-taudplay}
OUT=tests/out/fuzz
mkdir -p "$OUT"
first=${1:-1}; last=${2:-20}; secs=${3:-20}
fail=0
s=$first
while [ "$s" -le "$last" ]; do
  f="$OUT/fuzz$s.taud"
  node tests/gen_fuzz.mjs "$JS" "$s" "$f" 2>/dev/null
  n=$(./taudplay "$f" | grep -c '^  \[')
  k=0
  while [ "$k" -lt "$n" ]; do
    for mode in fold binaural; do
      extra=""; [ "$mode" = binaural ] && extra="--binaural"
      node tests/ref_render.mjs "$JS" "$f" "$k" "$secs" "$OUT/r.f32" $mode 2>"$OUT/r.err" || { echo "seed $s song $k ($mode): JS failed: $(tail -1 $OUT/r.err)"; continue; }
      ./taudplay "$f" -s "$k" -t "$secs" --raw "$OUT/c.f32" $extra 2>/dev/null || { echo "seed $s song $k ($mode): C failed"; fail=1; continue; }
      printf 'seed %s song %s (%s):' "$s" "$k" "$mode"
      ./tests/compare "$OUT/r.f32" "$OUT/c.f32" || fail=1
    done
    k=$((k + 1))
  done
  s=$((s + 1))
done
exit $fail
