# Holotoner Web Wrapper

This directory will contain the production browser wrapper around
`holotoner_core`. It is intentionally documentation-only until the shared
image-driven DSP contract is ready; there is no separate JavaScript audio
implementation to drift away from the native product.

The accepted boundary is:

- JavaScript owns file selection, responsive UI and rendering.
- A worker or the main UI thread decodes images and builds immutable control
  data away from audio rendering.
- AudioWorklet owns real-time audio scheduling.
- The C++20 core is compiled to WebAssembly and performs the same geometry and
  DSP as the native formats.
- User-selected images and audio remain local.
- GitHub Pages deployment must work from a project subpath and expose a visible
  link to corresponding source.

The first web production release includes a redistributable Holotoner demo loop
and local audio-file selection. Camera and microphone input are later complete
features, not hidden dependencies of the initial workflow.
