cmake_minimum_required(VERSION 3.10)
project(taudplay C)

# Bit-exact output needs every multiply and add rounded on its own:
# no fused multiply-add, no fast-math.
set(TAUDPLAY_SOURCES
    src/jsmath.c src/tables.c src/inst.c src/samplemod.c src/sampler.c
    src/envelope.c src/filter.c src/fm.c src/opl.c src/psg.c src/formant.c src/spatial.c src/binaural.c
    src/hrir_sadie.c src/mastering.c src/trigger.c src/effects.c src/row.c
    src/tick.c src/mixer.c src/engine.c src/parse.c src/zstd.c src/inflate.c
    src/resample.c src/player.c)

add_library(taudplay ${TAUDPLAY_SOURCES})
target_include_directories(taudplay PUBLIC include)
set_property(TARGET taudplay PROPERTY C_STANDARD 99)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(taudplay PRIVATE -ffp-contract=off -fno-fast-math)
elseif(MSVC)
  target_compile_options(taudplay PRIVATE /fp:precise)
endif()
find_library(MATH_LIB m)
if(MATH_LIB)
  target_link_libraries(taudplay PUBLIC ${MATH_LIB})
endif()

option(TAUDPLAY_LIBM_POW "Use the platform's pow instead of the bundled V8 one (not bit-exact)" OFF)
if(TAUDPLAY_LIBM_POW)
  target_compile_definitions(taudplay PRIVATE TAUD_LIBM_POW)
endif()

add_executable(taudplay-cli tools/taudplay.c)
set_target_properties(taudplay-cli PROPERTIES OUTPUT_NAME taudplay)
target_link_libraries(taudplay-cli PRIVATE taudplay)
