# Holotoner

Holotoner is an open-source image-driven stereo instrument. A directly
manipulated image plane and a read-only polar analyser show the same spatial
state that the audio engine renders.

The repository is under active development and does not yet contain a tagged
release. Development builds are not presented as production releases. Every
tagged Holotoner version will be a complete production version, even while the
instrument continues to evolve.

## Repository shape

- `core/` — portable C++20 geometry and DSP; no JUCE or browser dependency.
- `plugin/` — JUCE wrapper for Standalone, VST3 and AU development builds.
- `tests/` — deterministic core checks.
- `web/` — reserved for the production web wrapper once it consumes the shared
  core through AudioWorklet and WebAssembly.

The private product and process documents are deliberately outside this Git
repository. Public documentation belongs here.

## Current development foundation

The portable core currently establishes five invariants:

1. one canonical three-axis plane projection for future native and web views;
2. a smoothed signed mid/side baseline driven by that projection: a centred
   plane at 90 degrees yaw is mono, 180 degrees reverses the side channel and
   horizontal position balances the processed result;
3. an allocation-free test-source player that resamples and seamlessly loops
   immutable audio loaded away from the audio thread;
4. deterministic RGBA-to-mask analysis for Off, luma, alpha and individual
   colour channels, including bounded gain, fill, normalize, invert and amount
   behavior that preserves transparency;
5. a fixed-latency image spatializer that maps the projected mask over
   logarithmic frequency, can create stereo from mono, preserves the yaw 90
   mono and yaw 180 reversal endpoints, and crossfades transform, target and
   immutable mask changes without allocating in the render path.

This is a listening-capable development build, not a tagged release. The web
application, broader analyser detail, licensed demo loop and full release
verification remain before the first release.

## Build the core and tests

Requirements: CMake 3.25 or newer and a C++20 compiler.

```sh
cmake -S . -B build-core \
  -DCMAKE_BUILD_TYPE=Release \
  -DHOLOTONER_BUILD_PLUGIN=OFF
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure
```

## Build native formats

The native development target uses JUCE 9.0.2, fetched by CMake, and builds
Standalone, VST3 and AU on macOS.

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --parallel
```

The current JUCE wrapper is an unreleased development build around the tested
image spatializer. Its Standalone target starts with test clicks and supports
an off-thread audio-file loader, host-transport-aware playback and common
source controls. Its editor uses the intended side-by-side interaction: an
automatable image plane controls the canonical transform, while a read-only
polar panel shows the same projection, selected mask, input/output level and
output correlation.

Image import accepts PNG, JPEG and GIF away from the audio thread, bounds and
normalizes the image, embeds it in session state, and publishes a fixed 64 x 64
mask through a lock-free handover. The image controls select Off, luma, alpha,
red, green or blue; adjust gain and fill; optionally normalize or invert; and
send a smoothed amount to Off, Place or Level. Place maps horizontal image
energy to frequency-dependent stereo position and phase texture. Level maps
vertical brightness to frequency-dependent gain. Geometry remains independent
of pixel analysis in both cases.

The spectral path uses a 2048-sample FFT with four-times overlap, covers 40 Hz
to 16 kHz on a logarithmic image axis, and reports a fixed latency of 2047
samples to the host. Dry is delayed to remain phase-aligned with Wet.

## Development verification

The deterministic core suite exercises 44.1, 48 and 96 kHz; whole and split
blocks of 1, 31, 97 and 512 samples; geometry endpoints; delayed Dry; image
mask bounds; mono-to-stereo placement; and a fully black Level mask. The
Standalone, VST3 and AU targets are built as universal arm64/x86_64 binaries.
The VST3 passes pluginval at strictness level 5, including editor, state,
automation, bus-layout and 44.1/48/96 kHz processing checks.

An arm64 Release measurement on 11 October 2026 rendered 60 seconds of the
image spatializer at 48 kHz with 512-sample blocks in 0.45 seconds. That is
0.75% of one real-time stream's wall-clock budget on the test machine; it is a
development reference, not a cross-device performance guarantee.

## Licence

Copyright (C) 2026 Janne Särkelä.

Holotoner is free software licensed under the GNU Affero General Public License
version 3 only (`AGPL-3.0-only`). JUCE is obtained separately and is available
under AGPLv3 or a commercial JUCE licence; see JUCE's own licence before
building or distributing native formats.
