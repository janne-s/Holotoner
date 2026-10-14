# Holotoner Web

The browser version of [Holotoner](../README.md). Play the demo or load local
audio. Images and session files stay on your device.

## Run locally

From the repository root:

```sh
python3 -m http.server 8000
```

Open [localhost:8000/web/](http://localhost:8000/web/). The WASM engine is included.
Use HTTPS when hosting elsewhere.

## Build

Activate Emscripten 4.0.22, then run:

```sh
./web/build.sh
```

The browser uses AudioWorklet and the shared C++ engine. No npm install or
Node.js runtime is needed.

## GitHub Pages

Set Settings → Pages → Source to **GitHub Actions**. The Pages workflow builds,
tests and deploys `web/` when `main` or `master` changes.

## Tests

```sh
node web/tests/engine.mjs
```

For browser checks, serve the app on port 8765 and start a separate Chrome with
`--headless=new --mute-audio --remote-debugging-port=9236`, then run:

```sh
node web/tests/browser.mjs
```

## License

[AGPL-3.0-only](../LICENSE). [Third-party notices](THIRD_PARTY.md).
