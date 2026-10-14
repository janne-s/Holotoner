#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${EMXX:=em++}"
"$EMXX" -std=c++20 -O3 -DNDEBUG -I. web/bridge.cpp \
 core/dsp/Fft.cpp core/dsp/ImageSpatializer.cpp core/dsp/PhaseMap.cpp \
 core/dsp/SpaceCues.cpp core/geometry/PlaneProjection.cpp \
 core/image/ImageMask.cpp core/image/ShapeGenerator.cpp \
 --no-entry -s STANDALONE_WASM=1 -s FILESYSTEM=0 \
 -s INITIAL_MEMORY=33554432 -s ALLOW_MEMORY_GROWTH=0 -s STACK_SIZE=1048576 \
 -o web/assets/holotoner.wasm
