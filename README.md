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

The portable core currently establishes three invariants:

1. one canonical three-axis plane projection for future native and web views;
2. a smoothed signed mid/side baseline where a centred plane at 90 degrees yaw
   is mono and 180 degrees reverses the side channel;
3. an allocation-free test-source player that resamples and seamlessly loops
   immutable audio loaded away from the audio thread.

This is foundation work, not a released feature set. Image import, the final
spectral mono-to-stereo path, direct-manipulation UI, the full polar analyser,
the Standalone source controls and the web application must be complete before
the first release.

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

The current JUCE wrapper is intentionally a development shell around the tested
baseline. Its Standalone target starts with a test click, and the processor has
the off-thread file-loader and host-transport-aware playback path. The source
controls and the agreed two-panel editor are the next development stage; this
is not yet a Holotoner release.

## Licence

Copyright (C) 2026 Janne Särkelä.

Holotoner is free software licensed under the GNU Affero General Public License
version 3 only (`AGPL-3.0-only`). JUCE is obtained separately and is available
under AGPLv3 or a commercial JUCE licence; see JUCE's own licence before
building or distributing native formats.
