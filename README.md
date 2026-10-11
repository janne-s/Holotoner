# Holotoner

Holotoner is an open-source, image-driven stereo instrument. The image plane
is the controller; the read-only **Polar Sample** analyser measures actual
L/R audio, rather than drawing a synthetic picture of frequency bands.

The repository is under active development and has no tagged release yet.
Every tagged version will be a complete production version.

## Try the native development build

Build Standalone, VST3 and AU on macOS with CMake 3.25+ and a C++20 compiler.
JUCE 9.0.2 is fetched and pinned by CMake.

```sh
cmake -S . -B build/native-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-release --parallel
open build/native-release/plugin/Holotoner_artefacts/Release/Standalone/Holotoner.app
```

Use that exact app path: older build directories may contain an earlier editor.
Builds never install plug-ins into your DAW directories automatically.

1. Load an audio file with **Audio...** and an image with **Image...**.
   Test clicks are available, but sustained audio reveals the image better.
2. Select **Phase map** and **XY rectangle**, with Amount and Wet at 100%.
   An older saved session may still select Spectral place or Spectral level.
3. Begin with Gain 1 and Fill 0. Use Luma, Normalize, Threshold and optionally
   B/W to isolate a recognizable shape. The left panel shows the exact
   processed 64 × 64 mask, not the unprocessed colour photograph.
4. Drag the centre to move, the white handle to yaw/pitch, the pink handle to
   scale, and the top handle to roll. Geometry remains independent of pixels.
5. Choose **Polar warp** for a curved placement instead of a straight rectangle.
   Scale extends to 4×; Cartesian content outside the semicircle is clipped.

Fill 100% deliberately makes all opaque pixels equally active. Saturating Gain
can also erase image differences. The processed preview makes this visible.
Original image aspect is preserved until you scale the two axes independently.

## What the image does to audio

Phase map compiles a brightness-weighted distribution of the full 2D mask
off-thread. Fixed image samples define 33 smoothly interpolated phase/stereo
anchors across 0–16 kHz. Projection controls stereo direction; radius controls
relative/common phase, NOT audio level. Each frequency uses a unitary mid/side
rotation after the geometric width baseline. There is no moving image scan,
polarity-based replacement waveform, amplitude envelope or free-running sound.

The source spectrum and dynamics remain the audio material. At rest the image
defines a static spectral phase field; changing image, geometry or projection
smooths the coefficients over 20 ms. Geometric narrowing can remove side energy,
and partial Dry/Wet can cause phase cancellation; those are not tremolo.

The faint projected image is a spatial target, not a promise of pixel-perfect
audio reproduction. A single sine produces a line/ellipse; broadband material
reveals more of the image-derived field. The output shape can be less literal
than the former waveform scanner. No fake image points are added to the measured
output to compensate for this. Listening judgment remains necessary.

The analyser folds negative sample polarity into the upper semicircle.
Direction follows the L/R relationship; radius follows sample level on a
−60..0 dB scale. Its labelled +14 dB display gain applies equally to measured
input and output; it does not increase audio gain. Grey is the target/input;
pink is measured output. Source level changes the figure's radial reach.
The correlation bar is measured from output audio.

**Spectral place** and **Spectral level** remain explicit alternatives.
They map the image's vertical axis to logarithmic frequency, using a
2048-sample FFT with four-times overlap. These alternatives do not reconstruct
the full image. **Off** disables image processing while retaining plane geometry.
All paths, including Dry, use the same 2047-sample latency.

Image import accepts PNG, JPEG and GIF off-thread. Images are normalized and
embedded in versioned session state, so the original file is not required to
reopen a session. State version 5 replaces the unreleased waveform scanner with
the spatial phase field. Versions 2–4 remain readable with their images and
controls; old Phase map sessions use the corrected DSP, not the rejected scanner.
Media stays local.

## Repository and verification

Only this App directory is the public repository. Private product and process
documents remain outside it.

- `core/`: portable C++20 DSP, geometry and immutable image maps.
- `plugin/`: shared JUCE Standalone, VST3 and AU wrapper/editor.
- `tests/`: deterministic core tests and an explicit desktop/native harness.
- `web/`: documented boundary for the future shared-core WASM wrapper.

Build and test without JUCE:

```sh
cmake -S . -B build/core-release -DCMAKE_BUILD_TYPE=Release -DHOLOTONER_BUILD_PLUGIN=OFF
cmake --build build/core-release --parallel
ctest --test-dir build/core-release --output-on-failure
```

The core tests cover 44.1/48/96 kHz, blocks 1/31/97/512, exact delayed Dry,
geometry endpoints, mono-to-stereo, silence, deterministic masks and image
changes. Phase-map tests verify source linearity, stereo power, and pure-tone
purity/stability at 110/440/1760/7040 Hz across all three sample rates. Residual
energy outside the original tone must be below −50 dB and fitted stereo power
modulation below 0.5%; power must stay within 2% at the centred mono-input test.
These replace the old scanner's pixel-occupancy criterion: spatial sound takes
priority over exact waveform drawing.

Run the GUI/native harness in a desktop session; it does not open an audio device:

```sh
cmake --build build/native-release --target holotoner-native-tests --parallel
build/native-release/tests/holotoner-native-tests
```

It checks embedded-image/parameter round-trips, file-source playback, actual
sample history, and renders a diagnostic editor image to
`/tmp/holotoner-phase-editor.png`. Its media fixtures are original, temporary
test data, not a bundled demo.

Standalone, VST3 and AU build as universal arm64/x86_64 binaries. VST3 passes
pluginval strictness 5. The native harness measured 60 seconds of Phase map,
including sample history, in 0.58 seconds on 11 October 2026 (about 0.97% of
one real-time stream's wall-clock budget). Timings are machine-specific, not
a cross-device performance guarantee.

The browser application, licensed demo loop and user listening validation
remain development work before the first tagged release.

## Licence

Copyright (C) 2026 Janne Särkelä.

Holotoner is free software licensed under the GNU Affero General Public License
version 3 only (`AGPL-3.0-only`). JUCE is obtained separately and is available
under AGPLv3 or a commercial JUCE licence; see JUCE's own licence before
building or distributing native formats.
