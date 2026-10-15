# taudplay — portable C Taud replayer.
#
#   make            static library + CLI
#   make check      render the test corpus and compare against the JS library
#
# -ffp-contract=off is REQUIRED for bit-exact output: a fused multiply-add
# rounds once where the reference rounds twice. Never build with -ffast-math.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -ffp-contract=off -Iinclude
LDLIBS  += -lm

SRCS = src/jsmath.c src/tables.c src/inst.c src/samplemod.c src/sampler.c src/envelope.c \
       src/filter.c src/fm.c src/opl.c src/formant.c src/spatial.c src/binaural.c src/hrir_sadie.c \
       src/mastering.c src/trigger.c src/effects.c src/row.c src/tick.c \
       src/mixer.c src/engine.c src/parse.c src/zstd.c src/inflate.c \
       src/resample.c src/player.c
OBJS = $(SRCS:.c=.o)

all: libtaudplay.a taudplay

libtaudplay.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

taudplay: tools/taudplay.c libtaudplay.a
	$(CC) $(CFLAGS) -o $@ tools/taudplay.c libtaudplay.a $(LDLIBS)

tests/compare: tests/compare.c libtaudplay.a
	$(CC) $(CFLAGS) -o $@ tests/compare.c libtaudplay.a $(LDLIBS)

src/%.o: src/%.c src/taud_internal.h include/taudplay.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests/jsmath_check: tests/jsmath_check.c src/jsmath.c
	$(CC) $(CFLAGS) -o $@ tests/jsmath_check.c src/jsmath.c $(LDLIBS)

# Differential tests against the JavaScript library (needs Node and a
# checkout of microtone-taudplay; JS=path overrides ../microtone-taudplay).
JS ?= ../microtone-taudplay
check: all tests/compare tests/scenario tests/jsmath_check
	mkdir -p tests/out
	node tests/jsmath_ref.mjs 200000 > tests/out/jsmath.bin && ./tests/jsmath_check tests/out/jsmath.bin
	JS=$(JS) sh tests/run.sh
	JS=$(JS) sh tests/scenario.sh $(JS)/test/corpus/WHEN.taud 48000 0
	JS=$(JS) sh tests/scenario.sh $(JS)/test/corpus/slumberjack.taud 44100 1
	JS=$(JS) sh tests/fuzz.sh 1 12 8
	FUZZ_INSTS=formant node tests/gen_fuzz.mjs $(JS) 3 tests/out/formant.taud
	JS=$(JS) sh tests/scenario.sh tests/out/formant.taud 44100 0
	FUZZ_INSTS=opl node tests/gen_fuzz.mjs $(JS) 6 tests/out/opl.taud
	JS=$(JS) sh tests/scenario.sh tests/out/opl.taud 22050 1

clean:
	rm -f $(OBJS) libtaudplay.a taudplay tests/compare tests/scenario tests/robust tests/jsmath_check
	rm -rf tests/out

.PHONY: all check clean robust

# Corrupted-input stress test under AddressSanitizer + UBSan.
robust: tests/robust.c $(SRCS)
	$(CC) -std=c99 -g -O1 -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	    -Iinclude -o tests/robust tests/robust.c $(SRCS) $(LDLIBS)

tests/scenario: tests/scenario.c libtaudplay.a
	$(CC) $(CFLAGS) -o $@ tests/scenario.c libtaudplay.a $(LDLIBS)
