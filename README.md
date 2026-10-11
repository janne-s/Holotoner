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

Phase map now has two cooperating stages. The spectral stage spreads the
original audio with image-derived stereo/phase relationships and bounded,
slow phase drift (0.11/0.17 Hz, independent of pitch and source level), following
Spectral Space's phase-diffusion principle. It does not synthesize a carrier.

The image stage projects the full mask into a 64-radius × 129-direction
steering field OFF the audio thread. At the actual sample level, it finds image
support and rotates the existing mid/side sample pair towards it. This retains
L²+R² at every sample: pixels cannot force audio level, gate empty rows or
replace the waveform with its sign/envelope. Empty radial rows leave the
direction alone. A narrow seam fade avoids a jump where Polar Sample folds
negative polarity. Field/geometry changes are smoothed over 20 ms.

This second stage is nonlinear spatial steering, not a transparent all-pass
filter: it can colour the waveform and introduce harmonics. Retained combined
stereo energy does NOT imply unchanged individual channels, unchanged mono,
or a guarantee of no audible pumping. Subjective listening still matters.
Amount reduces both steering and phase-field influence; Wet blends aligned Dry.

The image now constrains real output points, not just 33 frequency settings.
Broadband material reveals more of the shape; source level determines which
radii are available. The field never invents level to fill missing portions of
the picture. Some spill remains around the polarity seam, outside image radial
support, and during transitions. No fake points are added to measured output.

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
reopen a session. State version 6 adds the projected image steering field.
Versions 2–5 remain readable with their images and controls; old Phase map
sessions use the corrected DSP, not the rejected scanner or fixed-only field.
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
changes. Image-off tests verify source linearity and pure-tone purity at
110/440/1760/7040 Hz. They do NOT claim that nonlinear image steering is
harmonic-free. The image-on test measures actual L/R samples from a 16-partial
source: at least 90% of points in the tested radial range must hit the image
within two mask pixels, with both main parts of the asymmetric figure visited.
Measured hits were about 98%, versus 22% without image steering. Steering must
retain instantaneous stereo power within 1e-7 of the same phase field without
steering (measured error below 5e-9).

Run the GUI/native harness in a desktop session; it does not open an audio device:

```sh
cmake --build build/native-release --target holotoner-native-tests --parallel
build/native-release/tests/holotoner-native-tests
```

It checks embedded-image/parameter/field round-trips, geometry-only field
rebuilds, file-source playback, actual
sample history, and renders a diagnostic editor image to
`/tmp/holotoner-phase-editor.png`. Its media fixtures are original, temporary
test data, not a bundled demo.

Standalone, VST3 and AU build as universal arm64/x86_64 binaries. VST3 passes
pluginval strictness 5. The native harness measured 60 seconds of Phase map,
including sample history, in 0.68 seconds on 11 October 2026 (about 1.14% of
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
