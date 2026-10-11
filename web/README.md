# Holotoner Web Wrapper

This directory will contain the production browser wrapper around
`holotoner_core`. It is intentionally documentation-only until the shared
image-driven DSP contract has been validated through native listening; there
will be no separate JavaScript audio implementation to drift away from the
native product. The shared core already contains the deterministic image mask,
plane projection, brightness-weighted phase-map trajectory, actual L/R scope
mapping and fixed-latency spatializer intended for the WebAssembly
build.

The accepted boundary is:

- JavaScript owns file selection, responsive UI and rendering.
- A worker or the main UI thread decodes images to straight-alpha RGBA away
  from audio rendering; the shared C++ core builds the same fixed-size mask as
  the native wrapper.
- AudioWorklet owns real-time audio scheduling.
- The C++20 core is compiled to WebAssembly and performs the same geometry and
  DSP as the native formats.
- Phase map is source-driven waveform reshaping, not transparent panning.
  XY rectangle, Polar warp, processed-mask preview, threshold and B/W must
  retain the native semantics; the analyser must measure actual L/R samples.
- User-selected images and audio remain local.
- GitHub Pages deployment must work from a project subpath and expose a visible
  link to corresponding source.

The first web production release includes a redistributable Holotoner demo loop
and local audio-file selection. Camera and microphone input are later complete
features, not hidden dependencies of the initial workflow.
