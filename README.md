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
2. Select **XY rectangle**, with Amount and Wet at 100%. Phase map is the
   only processing engine; there is no mode selector.
3. Begin with Gain 1 and Fill 0. Use Luma, Normalize, Threshold and optionally
   B/W to isolate a recognizable shape. The left panel shows the exact
   processed 64 × 64 mask, not the unprocessed colour photograph.
4. Drag the centre to move, the white handle to yaw/pitch, the pink handle to
   scale, and the top handle to roll. Geometry remains independent of pixels.
5. Choose **Polar warp** for a curved placement instead of a straight rectangle.
   Scale extends to 4×; Cartesian content outside the semicircle is clipped.

### Text and Motion

All image controls are visible on the left, audio controls on the right.
Knobs use Spectral Space's small bipolar arc, horizontal/vertical drag and
double-click reset. **Text** opens a row over the image view without resizing
the window. Enter an emoji, emoticon or text, then press Return or **Use text**.
Text is a bitmap source for the same phase-map engine, not another processing
mode. **Whole** shows everything. **Characters** plays visible grapheme clusters
(joined emoji, skin tones, flags and combining marks stay together); **Words**
splits on whitespace. Blank clusters are skipped. Select **Frame** manually,
or enable **Cycle**. The limits are 256 Unicode code points and 32 playback
frames; oversized input reports an error and keeps the previous source.
Loading **Image...** switches back to an ordinary image.

Each Rotate X/Y/Z, Move X/Y and Scale X/Y knob has its own **Rate + ♪** directly
below it. Multiple axes can move simultaneously at independent Hz/note values.
Rate 0 with ♪ off is stationary. **Swing** adds a sinusoidal offset; rotation depth 100% means ±180°,
position depth 100% means ±1, and scale depth 100% means half-to-double scale.
**Spin** makes one full turn per cycle for rotation (Depth is not used there);
position and scale still swing. Shape and Depth are shared across active axes.
The base plane is unchanged: gestures and DAW automation remain independent
of the internal motion offset. Position and scale stay within their bounds.
All base geometry and new motion/playback parameters are exposed to the host;
rotation display names are Rotation X/Y/Z, with the existing pitch/yaw/roll IDs.

Each rate has Spectral Space's **♪** switch: off uses Hz; on replaces the rate
with its note-value selector, including triplets and dotted notes. One Motion
cycle or one text step lasts the selected note. In a DAW the clock follows
host BPM and PPQ, including loops/seeks and negative preroll. Missing host PPQ
falls back to an audio-sample beat accumulator. Standalone has **Tempo** in BPM
and Play/Stop/Restart controls both local clocks. A whole note always means
four quarter-note beats, not an arbitrary time-signature bar.

Text rasterization and image-coordinate support maps are compiled on a worker,
never on the audio thread. Geometry, independent motion phases and projection
are consumed directly by audio, with 20 ms smoothing and continuous coefficient
interpolation between FFT hops. They do not wait for a GUI-timer map rebuild.
The old 64-pose motion banks are removed. Each frame stores a fixed 32 KiB
nearest-support map, with no multiplying pose-bank memory cost. Text is fitted
to visible ink bounds instead of leaving a small glyph in a mostly empty square.
Very rapid motion can still blur the figure through smoothing and scope history.
Character segmentation currently uses macOS's composed-character API;
non-macOS builds explicitly reject Characters rather than split emoji incorrectly.

Fill 100% deliberately makes all opaque pixels equally active. Saturating Gain
can also erase image differences. The processed preview makes this visible.
Original image aspect is preserved until you scale the two axes independently.

## What the image does to audio

Phase map now has two cooperating stages. The spectral stage spreads the
original audio with image-derived stereo/phase relationships and bounded,
slow phase drift (0.11/0.17 Hz, independent of pitch and source level), following
Spectral Space's phase-diffusion principle. It does not synthesize a carrier.

The image stage compiles nearest occupied points in the full 64 × 64 mask
OFF the audio thread. At the actual sample level, a bounded alternating
projection queries that immutable map through the current continuous plane
transform and finds image support at the sample's original radius. It rotates
the existing mid/side sample pair towards that direction. This retains
L²+R² at every sample: pixels cannot force audio level, gate empty rows or
replace the waveform with its sign/envelope. Unsupported radii leave the
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
input and output; it does not increase audio gain. Grey is measured input;
pink is measured output. No target image, mask dots or shadow picture are drawn
in the polar view. The processed image preview exists only on the left.
Source level changes the figure's radial reach.
The correlation bar is measured from output audio.

The engine uses a 2048-sample FFT with four-times overlap and 2047 samples of
latency. Dry is delayed by the same amount. Amount 0 or Channel Off disables
image influence while retaining plane geometry; Wet 0 restores aligned Dry.
The retired Spectral place/level algorithms and mode parameter are removed,
not hidden behind the simplified editor.

Image import accepts PNG, JPEG and GIF off-thread. Images are normalized and
embedded in versioned session state, so the original file is not required to
reopen a session. State version 9 stores text metadata and every rendered
frame as PNG, so reopening does not depend on the rendering machine's fonts.
Version 8 text bitmaps are fitted to ink without re-rendering their fonts;
its single Motion target migrates to the corresponding independent axis
(old Scale maps to both scale axes). Version 7 removed the mode parameter. Versions 2–8
remain readable with their images and other controls: old Phase/Place/Level
sessions use the single phase-field engine; old mode Off becomes Amount 0.
Old spectral sound is not retained as a dormant compatibility implementation.
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

It checks embedded-image/text-frame/parameter/field round-trips, emoji
segmentation/rendering, stale text jobs, transport seek/pause and synced
Motion/text playback across blocks 1/31/97/512 at 44.1/48/96 kHz, legacy mode migration,
audio-driven geometry changes without GUI ticks, a pixel-identical silent polar view with/without
an image, file-source playback, actual
sample history, and renders a diagnostic editor image to
`/tmp/holotoner-phase-editor.png`. An A regression at Gain 0.02 and threshold
0.967 verifies a connected mask (958 active/connected pixels) and measured
output support on both legs (about 99% in the tested radial range). UI tests
check rotary controls and a non-resizing text overlay. Its media fixtures are original, temporary
test data, not a bundled demo.

Standalone, VST3 and AU build as universal arm64/x86_64 binaries. VST3 passes
pluginval strictness 5. The native harness measured 60 seconds of Phase map,
including sample history, in about 2.1 seconds (about 3.5% of
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
