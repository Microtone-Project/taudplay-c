#!/bin/sh
# Render every song of every corpus file through the JS library and through
# the C port, and compare the float output sample for sample.
#   sh tests/run.sh [JS_LIB_DIR] [FILES...]
JS=${JS:-../microtone-taudplay}
SECONDS_MAX=${SECONDS_MAX:-900}
OUT=${OUT:-tests/out}
mkdir -p "$OUT"
[ $# -gt 0 ] || set -- "$JS"/test/corpus/*.taud
fail=0
for f in "$@"; do
  n=$(${TAUDPLAY:-./taudplay} "$f" | grep -c '^  \[')
  s=0
  while [ "$s" -lt "$n" ]; do
    for mode in fold binaural; do
      b=$(basename "$f" .taud | tr ' ' _)
      ref="$OUT/$b.$s.$mode.ref.f32"; got="$OUT/$b.$s.$mode.c.f32"
      extra=""; [ "$mode" = binaural ] && extra="--binaural"
      node tests/ref_render.mjs "$JS" "$f" "$s" "$SECONDS_MAX" "$ref" $mode 2>/dev/null || { echo "$b song $s ($mode): JS render failed"; fail=1; continue; }
      ${TAUDPLAY:-./taudplay} "$f" -s "$s" -t "$SECONDS_MAX" --raw "$got" $extra 2>/dev/null
      printf '%s song %s (%s):' "$b" "$s" "$mode"
      ./tests/compare "$ref" "$got" || fail=1
      rm -f "$ref" "$got"
    done
    s=$((s + 1))
  done
done
exit $fail
