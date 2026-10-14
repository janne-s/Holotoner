import {instantiate, keys, notes, snapshotByteLength} from './wasm.js';
const $ = id => document.getElementById(id),
      clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const pink = '#ff4fae', green = '#3ccf9b', dim = '#8b919b', line = '#272b32';
const defaults = {
  position_x : 0,
  position_y : 0,
  scale_x : 1,
  scale_y : 1,
  pitch : 0,
  yaw : 0,
  roll : 0,
  image_projection : 0,
  image_amount : 1,
  image_edge : 1,
  image_smooth : 0,
  image_rest : 0,
  wet : 1,
  out_delta : 0,
  space_interfere : 0,
  space_echo : 0,
  space_feedback : 0,
  space_resonate : 0,
  image_channel : 1,
  image_gain : 1,
  image_fill : 0,
  image_normalize : 1,
  image_invert : 0,
  image_threshold : 0,
  image_binary : 0,
  tempo : 120,
  space_echo_time : .25,
  space_echo_time_sync : 0,
  motion_shape : 0,
  motion_depth : .5,
  text_frame : 1,
  text_play : 0,
  text_rate : .25,
  text_rate_sync : 0
};
for (const key of keys)
  if (!(key in defaults))
    defaults[key] = 0;
let state = {...defaults}, core, module, worker, node, context, source,
    audioBuffer, masks, previews = [], images = [],
    sourceMeta = {kind : 'shape', shape : 7, order : 3, name : 'Smiley'},
    sourceReady = false;
let playing = false, offset = 0, started = 0, job = 0, pendingJobs = new Map(),
    liveCorners, liveResponse, measured, activeFrame = 0, lastSnapshot = 0,
    initializing;
let snapshotBuffer, sourceVersion = 0, scopeVersion = 0;
const snapshotBuffers = [
  new ArrayBuffer(snapshotByteLength), new ArrayBuffer(snapshotByteLength)
];
let audioLabel = 'Demo';
const controls = new Map(), ranges = new Map();
const axisKeys = [
  'pitch', 'yaw', 'roll', 'position_x', 'position_y', 'scale_x', 'scale_y'
];
const parameters = () => Float32Array.from(keys.map(k => state[k]));
let statusTimer;
function status(message, error = false) {
  clearTimeout(statusTimer);
  $('status').textContent = message;
  $('status').style.color = error ? pink : dim;
  if (!error)
    statusTimer = setTimeout(() => $('status').textContent = '', 3000);
}
function sendState() {
  if (core)
    new Float32Array(core.memory.buffer, core.ht_buffer(4), 48)
        .set(parameters());
  node?.port.postMessage({type : 'params', values : parameters()});
}
function update(key, value) {
  state[key] =
      key === 'image_smooth' ? Math.round(value * 100000) / 100000 : value;
  sendState();
  refreshControl(key);
  if ([
        'image_channel', 'image_gain', 'image_fill', 'image_normalize',
        'image_invert', 'image_threshold', 'image_binary'
      ].includes(key))
    scheduleCompile();
}
function format(key, v) {
  if ([ 'pitch', 'yaw', 'roll' ].includes(key))
    return `${Math.round(v)}°`;
  if (key.startsWith('scale'))
    return `${v.toFixed(2)}×`;
  if (key.startsWith('position'))
    return v.toFixed(2);
  if (key === 'image_gain')
    return `${v.toFixed(2)}×`;
  if (key === 'image_smooth')
    return `${(v * 1000).toFixed(2)} ms`;
  if (key === 'space_echo_time')
    return `${Math.round(v * 1000)} ms`;
  return `${Math.round(v * 100)}%`;
}
const markers = new Map(), markerLefts = new Map();
const markerOf = key => {
  if (!markers.has(key))
    markers.set(key,
                controls.get(key).input.closest('.axis').querySelector('i'));
  return markers.get(key);
};
function refreshControl(key) {
  const c = controls.get(key);
  if (!c)
    return;
  c.input.value = state[key];
  c.output.textContent = format(key, state[key]);
  const proportion = (state[key] - c.min) / (c.max - c.min);
  c.input.style.setProperty('--turn', `${
                                          270 * (key === 'image_smooth'
                                                     ? Math.sqrt(proportion)
                                                     : proportion)}deg`);
  if (axisKeys.includes(key)) {
    const origin = (defaults[key] - c.min) / (c.max - c.min) * 100;
    const position = (state[key] - c.min) / (c.max - c.min) * 100;
    c.input.style.setProperty('--fill-start', `${Math.min(origin, position)}%`);
    c.input.style.setProperty('--fill-end', `${Math.max(origin, position)}%`);
    markerOf(key).hidden = true;
  }
}
function knob(parent, key, label, min = 0, max = 1, step = .001) {
  const cell = document.createElement('div');
  cell.className = 'knob';
  cell.innerHTML =
      `<label for="${key}">${label}</label><div class="knob-group"><input id="${
          key}" type="range" min="${min}" max="${max}" step="${
          step}" aria-label="${label}"></div><output for="${key}"></output>`;
  $(parent).append(cell);
  const input = cell.querySelector('input'),
        output = cell.querySelector('output');
  controls.set(key, {input, output, min, max});
  ranges.set(key, [ min, max ]);
  input.oninput = () => update(key, +input.value);
  input.ondblclick = () => update(key, defaults[key]);
  let drag;
  input.addEventListener('pointerdown', e => {
    e.preventDefault();
    input.focus();
    input.setPointerCapture(e.pointerId);
    drag = {x : e.clientX, y : e.clientY, value : state[key]};
  });
  input.addEventListener('pointermove', e => {
    if (drag) {
      const delta = ((drag.y - e.clientY) + (e.clientX - drag.x)) / 180;
      const origin = (drag.value - min) / (max - min);
      const fraction = clamp(
          (key === 'image_smooth' ? Math.sqrt(origin) : origin) + delta, 0, 1);
      update(key,
             min + (key === 'image_smooth' ? fraction * fraction : fraction) *
                       (max - min));
    }
  });
  input.addEventListener('pointerup', () => drag = null);
  input.addEventListener('pointercancel', () => drag = null);
  output.title = 'Click to enter a value';
  output.onclick = () => {
    const unitScale = key === 'image_smooth' ? 1000 : 1;
    const raw = prompt(`${label} (${min * unitScale}…${max * unitScale}${
                           unitScale === 1000 ? ' ms' : ''})`,
                       String(state[key] * unitScale));
    if (raw !== null && Number.isFinite(+raw))
      update(key, clamp(+raw / unitScale, min, max));
  };
  refreshControl(key);
}
knob('image-controls', 'image_gain', 'Gain', 0, 8, .01);
knob('image-controls', 'image_fill', 'Fill');
knob('image-controls', 'image_threshold', 'Threshold');
knob('image-controls', 'image_edge', 'Snap');
knob('image-controls', 'image_smooth', 'Smooth', 0, .005, .00001);
knob('image-controls', 'image_rest', 'Rest', -1, 1);
knob('image-controls', 'image_amount', 'Amount');
knob('space-controls', 'space_interfere', 'Interfere');
knob('space-controls', 'space_echo', 'Echo');
knob('space-controls', 'space_echo_time', 'Time', .04, 1.5);
const timeCell = controls.get('space_echo_time').input.closest('.knob');
timeCell.classList.add('synced-control');
const timeGroup = timeCell.querySelector('.knob-group');
timeGroup.insertAdjacentHTML(
    'beforeend',
    '<select id="echo-note" class="sync-note" aria-label="Echo note value" hidden></select><button id="echo-sync" aria-pressed="false" aria-label="Sync echo time to tempo">♪</button>');
knob('space-controls', 'space_feedback', 'Feedback');
knob('space-controls', 'space_resonate', 'Resonate');
knob('out-controls', 'wet', 'Wet');
const axisNames = [
  'Rotate X', 'Rotate Y', 'Rotate Z', 'Move X', 'Move Y', 'Scale X', 'Scale Y'
];
function fillNotes(select) {
  notes.slice(1).forEach((name, i) => {
    const o = new Option(name, String(i + 1));
    select.add(o);
  });
  select.value = '12';
}
axisKeys.forEach((key, i) => {
  const row = document.createElement('label');
  row.className = 'axis';
  const min = i < 3   ? -180
              : i < 5 ? -1
                      : .05,
        max = i < 3   ? 180
              : i < 5 ? 1
                      : 4;
  row.innerHTML = `<span>${
      axisNames[i]}</span><span class="axis-slider"><input id="${
      key}" type="range" min="${min}" max="${max}" step="${
      i < 3 ? .1 : .001}" aria-label="${
      axisNames[i]}"><i title="Actual audio-smoothed position"></i></span><output></output><input class="rate" type="number" min="0" max="8" step="0.01" value="0" aria-label="${
      axisNames[i]} rate in Hz"><select hidden aria-label="${
      axisNames[i]} note"></select><button type="button" aria-pressed="false" aria-label="Sync ${
      axisNames[i]} to tempo">♪</button>`;
  $('motion-axes').append(row);
  const input = row.querySelector('input'),
        output = row.querySelector('output'), rate = row.querySelector('.rate'),
        note = row.querySelector('select'), sync = row.querySelector('button');
  fillNotes(note);
  controls.set(key, {input, output, min, max});
  ranges.set(key, [ min, max ]);
  refreshControl(key);
  input.oninput = () => update(key, +input.value);
  input.ondblclick = () => update(key, defaults[key]);
  rate.onchange = () => {
    rate.value = clamp(+rate.value, 0, 8);
    update(key + '_rate', +rate.value);
  };
  note.onchange = () => update(key + '_rate_sync', +note.value);
  sync.onclick = e => {
    e.preventDefault();
    const on = state[key + '_rate_sync'] === 0;
    update(key + '_rate_sync', on ? +note.value : 0);
    refreshAll();
  };
});
function refreshAll() {
  for (const key of controls.keys())
    refreshControl(key);
  $('tempo').value = state.tempo;
  $('channel').selectedIndex = state.image_channel;
  for (const [id, key] of [
           [ 'normalize', 'image_normalize' ], [ 'invert', 'image_invert' ],
           [ 'binary', 'image_binary' ], [ 'delta', 'out_delta' ],
           [ 'text-cycle', 'text_play' ]])
    $(id).checked = !!state[key];
  $('motion-shape').selectedIndex = state.motion_shape;
  $('motion-depth').value = state.motion_depth;
  $('text-step').value = state.text_frame;
  $('text-rate').value = state.text_rate;
  axisKeys.forEach(key => {
    const row = controls.get(key).input.closest('.axis'),
          on = state[key + '_rate_sync'] > 0;
    row.querySelector('.rate').value = state[key + '_rate'];
    row.querySelector('.rate').hidden = on;
    row.querySelector('select').hidden = !on;
    if (on)
      row.querySelector('select').value = state[key + '_rate_sync'];
    row.querySelector('button').setAttribute('aria-pressed', on);
  });
  for (const [prefix, key] of [[ 'echo', 'space_echo_time_sync' ],
                               [ 'text', 'text_rate_sync' ]]) {
    const on = state[key] > 0;
    $(prefix + '-sync').setAttribute('aria-pressed', on);
    $(prefix + '-note').hidden = !on;
    if (on)
      $(prefix + '-note').value = state[key];
    if (prefix === 'text')
      $('text-rate').hidden = on;
    else {
      const time = controls.get('space_echo_time');
      time.input.hidden = on;
      time.output.hidden = on;
    }
  }
  $('xy').setAttribute('aria-pressed', state.image_projection === 0);
  $('polar').setAttribute('aria-pressed', state.image_projection === 1);
}
for (const prefix of ['echo', 'text']) {
  fillNotes($(prefix + '-note'));
  $(prefix + '-note').hidden = true;
  const key = prefix === 'echo' ? 'space_echo_time_sync' : 'text_rate_sync';
  $(prefix + '-sync').onclick = () => {
    update(key, state[key] ? 0 : +$(prefix + '-note').value);
    refreshAll();
  };
  $(prefix + '-note').onchange = () => update(key, +$(prefix + '-note').value);
}
$('channel').onchange = () =>
    update('image_channel', $('channel').selectedIndex);
for (const [id, key] of [[ 'normalize', 'image_normalize' ],
                         [ 'invert', 'image_invert' ],
                         [ 'binary', 'image_binary' ], [ 'delta', 'out_delta' ],
                         [ 'text-cycle', 'text_play' ]])
  $(id).onchange = () => update(key, +$(id).checked);
for (const [id, key, min, max] of [[ 'tempo', 'tempo', 20, 300 ],
                                   [ 'motion-depth', 'motion_depth', 0, 1 ],
                                   [ 'text-step', 'text_frame', 1, 32 ],
                                   [ 'text-rate', 'text_rate', 0, 8 ]])
  $(id).onchange = () => {
    const v = clamp(+$(id).value, min, max);
    $(id).value = v;
    update(key, v);
  };
$('motion-depth').ondblclick = () => {
  $('motion-depth').value = defaults.motion_depth;
  update('motion_depth', defaults.motion_depth);
};
$('motion-shape').onchange = () =>
    update('motion_shape', $('motion-shape').selectedIndex);
$('xy').onclick = () => {
  update('image_projection', 0);
  refreshAll();
};
$('polar').onclick = () => {
  update('image_projection', 1);
  refreshAll();
};
function toggle(id, button) {
  const open = $(id).hidden;
  $(id).hidden = !open;
  $(button).setAttribute('aria-expanded', open);
  if (id === 'motion')
    $(button).textContent = open ? 'Motion ▾' : 'Motion ▴';
}
$('motion-toggle').onclick = () => toggle('motion', 'motion-toggle');
for (const name of ['text', 'shape'])
  $(name + '-toggle').onclick = safely(async () => {
    const other = name === 'text' ? 'shape' : 'text';
    $(other + '-panel').hidden = true;
    $(other + '-toggle').setAttribute('aria-expanded', false);
    toggle(name + '-panel', name + '-toggle');
    if (name === 'shape' && !$(name + '-panel').hidden)
      await applyShape();
  });
// Motion only: every axis Rate and note off, Swing and Depth to defaults. The
// base geometry stays (Reset plane owns that).
$('reset-motion').onclick = () => {
  for (const key of ['motion_shape', 'motion_depth',
                     ...axisKeys.flatMap(k => [k + '_rate', k + '_rate_sync'])])
    state[key] = defaults[key];
  sendState();
  refreshAll();
};
$('reset-plane').onclick = () => {
  for (const key of axisKeys)
    state[key] = defaults[key];
  sendState();
  refreshAll();
};
let compileTimer, requestedImages = [], requestedMeta = sourceMeta;
function scheduleCompile() {
  clearTimeout(compileTimer);
  compileTimer = setTimeout(
      () => compile().catch(error => status(error.message, true)), 80);
}
function compile(candidateImages, meta) {
  if (candidateImages !== undefined) {
    requestedImages = candidateImages;
    requestedMeta = meta;
  } else if (!requestedImages.length && sourceMeta.kind !== 'shape') {
    requestedImages = images;
    requestedMeta = sourceMeta;
  }
  candidateImages = requestedImages;
  meta = requestedMeta;
  return new Promise((resolve, reject) => {
    const id = ++job;
    pendingJobs.set(id, {resolve, reject, meta});
    worker.postMessage({
      type : 'compile',
      id,
      params : parameters(),
      images : candidateImages,
      shape : meta.kind === 'shape' ? {kind : meta.shape, order : meta.order}
                                    : null
    });
  });
}
function installMasks() {
  if (!masks)
    return;
  if (core) {
    new Uint8Array(core.memory.buffer, core.ht_buffer(5), masks.length)
        .set(masks);
    core.ht_commit(0, previews.length);
  }
  node?.port.postMessage({type : 'masks', masks, count : previews.length});
}
async function setup() {
  if (!window.isSecureContext || !window.AudioWorkletNode ||
      !window.WebAssembly)
    throw Error(
        'Use a current browser on HTTPS or localhost with Web Audio and WebAssembly support.');
  const result =
      await fetch(new URL('./assets/holotoner.wasm', import.meta.url));
  if (!result.ok)
    throw Error(
        'Audio engine is missing. Build web/assets/holotoner.wasm first.');
  module = await WebAssembly.compile(await result.arrayBuffer());
  core = instantiate(module);
  sendState();
  worker = new Worker(new URL('./mask-worker.js', import.meta.url),
                      {type : 'module'});
  await new Promise((resolve, reject) => {
    worker.onerror = event => {
      const error =
          Error(event.message || 'Image worker stopped. Reload to restart.');
      reject(error);
      for (const pending of pendingJobs.values())
        pending.reject(error);
      pendingJobs.clear();
      status(error.message, true);
    };
    worker.onmessage = ({data}) => {
      if (data.type === 'ready')
        resolve();
      else if (data.type === 'error') {
        pendingJobs.get(data.id)?.reject(Error(data.message));
        pendingJobs.delete(data.id);
      } else if (data.type === 'masks') {
        const pending = pendingJobs.get(data.id);
        pendingJobs.delete(data.id);
        if (data.id !== job) {
          pending?.resolve();
          return;
        }
        images = data.images;
        requestedImages = images;
        masks = data.masks;
        previews = data.previews;
        ++sourceVersion;
        sourceMeta = pending.meta;
        sourceReady = true;
        activeFrame = clamp(activeFrame, 0, previews.length - 1);
        installMasks();
        $('text-step').max = previews.length;
        pending?.resolve();
      }
    };
    worker.postMessage({type : 'init', module});
  });
  await compile([], sourceMeta);
  $('play').disabled = false;
  status('');
}
async function initAudio() {
  if (node)
    return;
  if (initializing)
    return initializing;
  initializing = (async () => {
    context = new AudioContext({latencyHint : 'interactive'});
    await context.audioWorklet.addModule(
        new URL('./audio-worklet.js', import.meta.url));
    node = new AudioWorkletNode(context, 'holotoner', {
      numberOfInputs : 1,
      numberOfOutputs : 1,
      outputChannelCount : [ 2 ],
      channelCount : 2,
      channelCountMode : 'explicit',
      processorOptions : {module}
    });
    node.connect(context.destination);
    node.onprocessorerror = () => {
      stop();
      audioFailed = true;
      status('Audio engine stopped. Reload to restart.', true);
      $('play').disabled = true;
    };
    node.port.onmessage = ({data}) => {
      if (data.type === 'snapshot') {
        lastSnapshot = 0;
        // Silence and pauses repeat identical packets: count only new content.
        if (!measured || measured.length !== data.scope.length ||
            measured.some((v, i) => v !== data.scope[i]))
          ++scopeVersion;
        if (snapshotBuffer)
          snapshotBuffers.push(snapshotBuffer);
        snapshotBuffer = data.scope.buffer;
        measured = data.scope;
        liveCorners = data.corners;
        liveResponse = data.response;
        activeFrame = data.metrics[2];
        // Write the DOM only when a value changes: every write invalidates
        // style and layout, and this runs at the analyser rate.
        axisKeys.forEach((key, i) => {
          const control = controls.get(key), marker = markerOf(key);
          const hidden = Math.abs(data.plane[i] - state[key]) <=
                         .001 * (control.max - control.min);
          if (marker.hidden !== hidden)
            marker.hidden = hidden;
          const left = `${
              (100 * clamp((data.plane[i] - control.min) /
                               (control.max - control.min),
                           0, 1))
                  .toFixed(2)}%`;
          if (!hidden && markerLefts.get(key) !== left) {
            markerLefts.set(key, left);
            marker.style.left = left;
          }
        });
        const correlation = data.metrics[0].toFixed(2);
        if ($('correlation').value !== correlation) {
          $('correlation').value = correlation;
          $('correlation-tick').style.left = `${50 + 50 * +correlation}%`;
        }
        if (data.metrics[3] > 0)
          status('Engine recovered from invalid audio.', true);
      }
    };
    sendState();
    installMasks();
  })();
  try {
    await initializing;
  } catch (error) {
    node?.disconnect();
    node = null;
    await context?.close();
    context = null;
    throw error;
  } finally {
    initializing = null;
  }
}
async function loadAudio(file) {
  await initAudio();
  if (file.size > 100 * 1024 * 1024)
    throw Error('Choose an audio file below 100 MB.');
  const decoded = await context.decodeAudioData(await file.arrayBuffer());
  stop();
  audioBuffer = decoded;
  offset = 0;
  audioLabel = file.name;
  $('audio-name').textContent = audioLabel;
  status('Audio loaded.');
}
async function loadDemo() {
  await initAudio();
  const response = await fetch(new URL('./assets/demo.wav', import.meta.url));
  if (!response.ok)
    throw Error('Demo audio could not be loaded.');
  const decoded = await context.decodeAudioData(await response.arrayBuffer());
  stop();
  audioBuffer = decoded;
  offset = 0;
  audioLabel = 'Demo';
  $('audio-name').textContent = audioLabel;
  status('Demo loaded.');
}
// iOS treats Web Audio as ambient sound, which the silent switch mutes.
// Declaring the page's audio session as playback (Safari 16.4+) makes it media,
// which plays through the switch; older iOS takes the session from an HTML
// audio element, so there a silent one loops while sound plays.
let silence = null;
function declarePlayback() {
  if ('audioSession' in navigator) {
    try {
      navigator.audioSession.type = 'playback';
    } catch {
      // Not settable here.
    }
    return;
  }
  silence ??= silentLoop();
  silence.play().catch(() => {
    // Without a gesture or support, Web Audio plays as it would.
  });
}
function silentLoop() {
  const rate = 8000, samples = rate / 2, bytes = new Uint8Array(44 + samples),
        view = new DataView(bytes.buffer);
  const text = (at, value) =>
      [...value].forEach((char, i) => bytes[at + i] = char.charCodeAt(0));
  text(0, 'RIFF');
  view.setUint32(4, 36 + samples, true);
  text(8, 'WAVEfmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, rate, true);
  view.setUint32(28, rate, true);
  view.setUint16(32, 1, true);
  view.setUint16(34, 8, true);
  text(36, 'data');
  view.setUint32(40, samples, true);
  bytes.fill(128, 44);
  const audio =
      new Audio(`data:audio/wav;base64,${btoa(String.fromCharCode(...bytes))}`);
  audio.loop = true;
  audio.setAttribute('playsinline', '');
  return audio;
}
function stop() {
  if (source) {
    offset = (offset + (context.currentTime - started)) % audioBuffer.duration;
    source.stop();
    source.disconnect();
    source = null;
  }
  playing = false;
  node?.port.postMessage({type : 'transport', playing : false});
  $('play').textContent = '▶ Play';
}
async function play() {
  if (!audioBuffer)
    await loadDemo();
  await context.resume();
  source = context.createBufferSource();
  source.buffer = audioBuffer;
  source.loop = true;
  source.connect(node);
  started = context.currentTime;
  source.start(0, offset);
  playing = true;
  node.port.postMessage({type : 'transport', playing : true});
  $('play').textContent = 'Ⅱ Pause';
  status('');
}
function safely(fn) {
  return async (...args) => {
    try {
      await fn(...args);
    } catch (error) {
      status(error.message, true);
    }
  };
}
let audioBusy = false, audioFailed = false;
function audioAction(fn) {
  return safely(async (...args) => {
    if (audioBusy || audioFailed || !module)
      return;
    // First thing in the gesture: iOS accepts the session change only here.
    declarePlayback();
    audioBusy = true;
    const buttons = [ 'play', 'restart', 'demo', 'load-audio' ].map($);
    for (const button of buttons)
      button.disabled = true;
    try {
      await fn(...args);
    } finally {
      audioBusy = false;
      for (const button of buttons)
        button.disabled = audioFailed;
    }
  });
}
$('play').onclick = audioAction(async () => {
  if (playing) {
    stop();
    // Restart and loading stop only to play again: the silence ends on Pause.
    silence?.pause();
  } else
    await play();
});
$('restart').onclick = audioAction(async () => {
  const resume = playing;
  stop();
  offset = 0;
  node?.port.postMessage({type : 'transport', playing : false, reset : true});
  measured = null;
  if (resume)
    await play();
});
$('demo').onclick = audioAction(loadDemo);
$('load-audio').onclick = () => $('audio-file').click();
$('audio-file').onchange = audioAction(async () => {
  const file = $('audio-file').files[0];
  try {
    if (file)
      await loadAudio(file);
  } finally {
    $('audio-file').value = '';
  }
});
async function decodeImage(file) {
  if (file.size > 20 * 1024 * 1024)
    throw Error('Choose an image below 20 MB.');
  const bitmap = await createImageBitmap(file);
  try {
    const ratio = Math.min(1, 512 / Math.max(bitmap.width, bitmap.height));
    const canvas = document.createElement('canvas');
    canvas.width = Math.max(1, Math.round(bitmap.width * ratio));
    canvas.height = Math.max(1, Math.round(bitmap.height * ratio));
    const c = canvas.getContext('2d');
    c.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
    return {
      width : canvas.width,
      height : canvas.height,
      pixels :
          new Uint8Array(c.getImageData(0, 0, canvas.width, canvas.height).data)
    };
  } finally {
    bitmap.close();
  }
}
$('load-image').onclick = () => $('image-file').click();
$('image-file').onchange = safely(async () => {
  const file = $('image-file').files[0];
  if (file) {
    await compile([ await decodeImage(file) ],
                  {kind : 'image', name : file.name});
    status('Image loaded.');
  }
  $('image-file').value = '';
});
function textFrames(text, layout) {
  if ([...text ].length > 256)
    throw Error('Text supports at most 256 Unicode code points.');
  let frames;
  if (layout === 'Whole')
    frames = [ text ];
  else if (layout === 'Words')
    frames = text.split(/\s+/u);
  else {
    if (!Intl.Segmenter)
      throw Error('This browser needs Intl.Segmenter for Characters.');
    frames = [
      ...new Intl.Segmenter(undefined, {granularity : 'grapheme'}).segment(text)
    ].map(s => s.segment);
  }
  frames = frames.filter(s => s.trim());
  if (!frames.length || frames.length > 32)
    throw Error('Use nonblank text with at most 32 frames.');
  return frames;
}
function rasterText(text) {
  const canvas = document.createElement('canvas');
  canvas.width = 1024;
  canvas.height = 256;
  const c = canvas.getContext('2d', {willReadFrequently : true});
  c.font = 'bold 180px monospace';
  const w = c.measureText(text).width;
  if (w > 980)
    c.font = `bold ${180 * 980 / w}px monospace`;
  c.fillStyle = 'white';
  c.textBaseline = 'middle';
  c.fillText(text, 16, 128);
  const data = c.getImageData(0, 0, 1024, 256);
  let x0 = 1024, y0 = 256, x1 = 0, y1 = 0;
  for (let y = 0; y < 256; y++)
    for (let x = 0; x < 1024; x++)
      if (data.data[(y * 1024 + x) * 4 + 3] > 0) {
        x0 = Math.min(x0, x);
        x1 = Math.max(x1, x);
        y0 = Math.min(y0, y);
        y1 = Math.max(y1, y);
      }
  if (x1 < x0)
    throw Error('This text has no visible glyphs.');
  const out = document.createElement('canvas'),
        factor = 256 / Math.max(x1 - x0 + 1, y1 - y0 + 1);
  out.width = Math.max(1, Math.round((x1 - x0 + 1) * factor));
  out.height = Math.max(1, Math.round((y1 - y0 + 1) * factor));
  const o = out.getContext('2d');
  o.drawImage(canvas, x0, y0, x1 - x0 + 1, y1 - y0 + 1, 0, 0, out.width,
              out.height);
  return {
    width : out.width,
    height : out.height,
    pixels : new Uint8Array(o.getImageData(0, 0, out.width, out.height).data)
  };
}
$('text-form').onsubmit = safely(async e => {
  e.preventDefault();
  const text = $('text').value, layout = $('text-layout').value;
  const frames = textFrames(text, layout).map(rasterText);
  await compile(frames, {kind : 'text', text, layout, name : `Text: ${text}`});
  state.text_frame = 1;
  sendState();
  refreshAll();
  status('Text loaded / ' + frames.length + ' frame(s).');
});
async function applyShape() {
  const input = $('shape-order');
  if (!input.value || !input.validity.valid)
    return;
  const shape = $('shape-kind').selectedIndex, order = input.valueAsNumber;
  await compile([],
                {kind : 'shape', shape, order, name : $('shape-kind').value});
  status('Shape loaded.');
}
$('shape-kind').onchange = safely(applyShape);
$('shape-order').oninput = safely(applyShape);
// Sessions explicitly embed normalized straight-alpha pixels. No media is
// retained implicitly.
function base64(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i += 8192)
    s += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(s);
}
function unbase64(s) {
  const raw = atob(s);
  return Uint8Array.from(raw, c => c.charCodeAt(0));
}
$('save').onclick = safely(async () => {
  if (!sourceReady || pendingJobs.size)
    throw Error('Wait for the image to finish loading.');
  const session = {
    format : 'holotoner-web',
    version : 1,
    parameters : state,
    source : sourceMeta,
    frames : images.map(
        im =>
            ({width : im.width, height : im.height, rgba : base64(im.pixels)}))
  };
  const blob =
            new Blob([ JSON.stringify(session) ], {type : 'application/json'}),
        url = URL.createObjectURL(blob), a = document.createElement('a');
  a.href = url;
  a.download = 'holotoner-state.json';
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  status('Saved.');
});
function validateState(session) {
  if (session.format !== 'holotoner-web' || session.version !== 1)
    throw Error('Unsupported Holotoner web state.');
  if (!session.parameters || !session.source ||
      !Array.isArray(session.frames) || session.frames.length < 1 ||
      session.frames.length > 32)
    throw Error('Invalid session structure.');
  const restored = {};
  for (const key of keys) {
    const v = key === 'image_smooth' && !('image_smooth' in session.parameters)
                  ? 0
                  : session.parameters[key];
    if (typeof v !== 'number' || !Number.isFinite(v))
      throw Error('Invalid parameter: ' + key);
    let bounds = ranges.get(key) || [ 0, 1 ];
    if (key.endsWith('_sync'))
      bounds = [ 0, 23 ];
    else if (key.endsWith('_rate'))
      bounds = [ 0, 8 ];
    else if (key === 'image_channel')
      bounds = [ 0, 5 ];
    else if (key === 'tempo')
      bounds = [ 20, 300 ];
    else if (key === 'text_frame')
      bounds = [ 1, 32 ];
    if ((key.endsWith('_sync') ||
         [
           'image_channel', 'image_projection', 'motion_shape', 'text_frame',
           'text_play', 'out_delta', 'image_normalize', 'image_invert',
           'image_binary'
         ].includes(key)) &&
        !Number.isInteger(v))
      throw Error('Invalid discrete parameter: ' + key);
    if (v < bounds[0] || v > bounds[1])
      throw Error('Parameter out of range: ' + key);
    restored[key] = v;
  }
  const frames = session.frames.map(im => {
    if (!Number.isInteger(im.width) || !Number.isInteger(im.height) ||
        im.width < 1 || im.height < 1 || im.width > 512 || im.height > 512 ||
        typeof im.rgba !== 'string' || im.rgba.length > 1400000)
      throw Error('Invalid image frame.');
    const pixels = unbase64(im.rgba);
    if (pixels.length !== im.width * im.height * 4)
      throw Error('Invalid image data.');
    return {width : im.width, height : im.height, pixels};
  });
  if (!['image', 'text', 'shape'].includes(session.source.kind) ||
      typeof session.source.name !== 'string')
    throw Error('Invalid source metadata.');
  if (session.source.kind === 'shape' &&
      (!Number.isInteger(session.source.shape) || session.source.shape < 0 ||
       session.source.shape >= $('shape-kind').options.length ||
       !Number.isInteger(session.source.order) || session.source.order < 1 ||
       session.source.order > 8))
    throw Error('Invalid shape metadata.');
  if (session.source.kind === 'text' &&
      (typeof session.source.text !== 'string' ||
       !['Whole', 'Characters', 'Words'].includes(session.source.layout)))
    throw Error('Invalid text metadata.');
  return {restored, frames};
}
$('restore').onclick = () => $('state-file').click();
$('state-file').onchange = safely(async () => {
  const file = $('state-file').files[0];
  if (!file)
    return;
  if (file.size > 48 * 1024 * 1024)
    throw Error('Session exceeds the 48 MB limit.');
  const session = JSON.parse(await file.text()),
        {restored, frames} = validateState(session);
  const previous = state;
  state = restored;
  try {
    await compile(frames, {
      ...session.source,
      kind : session.source.kind === 'shape' ? 'image' : session.source.kind
    });
    sourceMeta = session.source;
    requestedMeta = sourceMeta;
  } catch (error) {
    state = previous;
    sendState();
    throw error;
  }
  sendState();
  refreshAll();
  if (sourceMeta.kind === 'text') {
    $('text').value = sourceMeta.text;
    $('text-layout').value = sourceMeta.layout;
  }
  if (sourceMeta.kind === 'shape') {
    $('shape-kind').selectedIndex = sourceMeta.shape;
    $('shape-order').value = sourceMeta.order;
  }
  status('State restored.');
  $('state-file').value = '';
});
// Canvas display uses the core's projected corners. The polar receives measured
// points only.
// Each canvas is redrawn only when what it shows changes; static artwork is
// rendered once per size. Sizes come from a ResizeObserver, so drawing never
// forces a synchronous layout.
const canvasSizes = new Map();
const resizes = new ResizeObserver(entries => {
  for (const entry of entries)
    canvasSizes.set(entry.target.id, entry.contentRect);
});
for (const id of ['plane', 'scope', 'response'])
  resizes.observe($(id));
function canvasSize(id) {
  const rect = canvasSizes.get(id) || $(id).getBoundingClientRect();
  return {
    w : rect.width,
    h : rect.height,
    dpr : Math.min(devicePixelRatio || 1, 2)
  };
}
const drawn = new Map();
function changed(id, key) {
  const {w, h, dpr} = canvasSize(id);
  key = `${w}x${h}@${dpr}|${key}`;
  if (drawn.get(id) === key)
    return false;
  drawn.set(id, key);
  return true;
}
function canvasContext(id) {
  const canvas = $(id), {w : cssW, h : cssH, dpr} = canvasSize(id),
                 w = Math.max(1, Math.round(cssW * dpr)),
                 h = Math.max(1, Math.round(cssH * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  const c = canvas.getContext('2d');
  c.setTransform(dpr, 0, 0, dpr, 0, 0);
  c.clearRect(0, 0, cssW, cssH);
  return {c, w : cssW, h : cssH, dpr};
}
const layers = new Map();
function cachedLayer(name, w, h, dpr, paint) {
  const key = `${w}x${h}@${dpr}`;
  let layer = layers.get(name);
  if (!layer || layer.key !== key) {
    const canvas = document.createElement('canvas');
    canvas.width = Math.max(1, Math.round(w * dpr));
    canvas.height = Math.max(1, Math.round(h * dpr));
    const c = canvas.getContext('2d');
    c.setTransform(dpr, 0, 0, dpr, 0, 0);
    paint(c);
    layers.set(name, layer = {key, canvas});
  }
  return layer.canvas;
}
const maskCanvases = new WeakMap();
function maskCanvas(mask) {
  let canvas = maskCanvases.get(mask);
  if (!canvas) {
    canvas = document.createElement('canvas');
    canvas.width = canvas.height = 64;
    const m = canvas.getContext('2d'), data = m.createImageData(64, 64);
    for (let i = 0; i < 4096; i++) {
      data.data[i * 4] = 255;
      data.data[i * 4 + 1] = 79;
      data.data[i * 4 + 2] = 174;
      data.data[i * 4 + 3] = Math.round(clamp(mask[i], 0, 1) * 255);
    }
    m.putImageData(data, 0, 0);
    maskCanvases.set(mask, canvas);
  }
  return canvas;
}
const coarsePointer = matchMedia('(pointer: coarse)');
let handles = {};
function planeDraw(corners) {
  const {c, w, h, dpr} = canvasContext('plane'),
        unit = Math.min(w * .34, h * .35), cx = w / 2, cy = h / 2;
  c.drawImage(cachedLayer('plane-grid', w, h, dpr, g => {
    g.strokeStyle = line;
    g.lineWidth = .5;
    for (let i = -4; i <= 4; i++) {
      g.beginPath();
      g.moveTo(cx + i * unit / 2, 0);
      g.lineTo(cx + i * unit / 2, h);
      g.stroke();
      g.beginPath();
      g.moveTo(0, cy + i * unit / 2);
      g.lineTo(w, cy + i * unit / 2);
      g.stroke();
    }
    g.strokeStyle = dim;
    g.beginPath();
    g.moveTo(cx - 5, cy);
    g.lineTo(cx + 5, cy);
    g.moveTo(cx, cy - 5);
    g.lineTo(cx, cy + 5);
    g.stroke();
  }), 0, 0, w, h);
  if (!corners)
    return;
  const points = [];
  for (let i = 0; i < 4; i++)
    points.push(
        {x : cx + corners[i * 3] * unit, y : cy - corners[i * 3 + 1] * unit});
  const mask = previews[activeFrame] || previews[0];
  if (mask) {
    const p0 = points[3], p1 = points[2], p2 = points[0];
    c.save();
    c.transform((p1.x - p0.x) / 64, (p1.y - p0.y) / 64, (p2.x - p0.x) / 64,
                (p2.y - p0.y) / 64, p0.x, p0.y);
    c.imageSmoothingEnabled = false;
    c.drawImage(maskCanvas(mask), 0, 0);
    c.restore();
  }
  c.strokeStyle = pink;
  c.lineWidth = 1;
  c.beginPath();
  points.forEach((p, i) => i ? c.lineTo(p.x, p.y) : c.moveTo(p.x, p.y));
  c.closePath();
  c.stroke();
  const centre = {
    x : points.reduce((s, p) => s + p.x, 0) / 4,
    y : points.reduce((s, p) => s + p.y, 0) / 4
  };
  // Match the native panel: clamp only the interaction points, leaving the
  // projected image unchanged. Drawing and hit testing use these same points.
  const inset = coarsePointer.matches ? 22 : 8;
  const constrain = p =>
      ({x : clamp(p.x, inset, w - inset), y : clamp(p.y, inset, h - inset)});
  handles = {
    centre : constrain(centre),
    rotate : constrain(points[2]),
    scale : constrain(points[1]),
    roll : constrain({
      x : (points[2].x + points[3].x) / 2,
      y : (points[2].y + points[3].y) / 2 - 22
    }),
    unit
  };
  for (const [name, p] of Object.entries(handles)) {
    if (!['rotate', 'scale', 'roll'].includes(name))
      continue;
    c.strokeStyle = name === 'scale' ? pink : green;
    c.lineWidth = 1;
    c.beginPath();
    if (name === 'roll')
      c.arc(p.x, p.y, 5, 0, Math.PI * 2);
    else if (name === 'scale')
      c.rect(p.x - 5, p.y - 5, 10, 10);
    else {
      c.moveTo(p.x, p.y - 6);
      c.lineTo(p.x + 6, p.y);
      c.lineTo(p.x, p.y + 6);
      c.lineTo(p.x - 6, p.y);
      c.closePath();
    }
    c.fillStyle = '#0d0f12';
    c.fill();
    c.stroke();
  }
}
function scopeDraw() {
  const {c, w, h, dpr} = canvasContext('scope'), r = Math.min(w * .44, h - 42),
                    cx = w / 2, cy = h - 22;
  c.drawImage(cachedLayer('scope-grid', w, h, dpr, g => {
    g.lineWidth = .6;
    g.font = '9px monospace';
    g.textAlign = 'center';
    for (let i = 1; i <= 4; i++) {
      g.strokeStyle = line;
      g.beginPath();
      g.arc(cx, cy, r * i / 4, Math.PI, Math.PI * 2);
      g.stroke();
      g.fillStyle = dim;
      g.fillText(`${- 14 - (4 - i) * 15} dB`, cx + 7, cy - r * i / 4 + 10);
    }
    for (let angle = -90; angle <= 90; angle += 45) {
      const a = angle * Math.PI / 180;
      g.strokeStyle = line;
      g.beginPath();
      g.moveTo(cx, cy);
      g.lineTo(cx + r * Math.sin(a), cy - r * Math.cos(a));
      g.stroke();
    }
    g.fillStyle = dim;
    g.fillText('MONO', cx, cy - r - 6);
    g.fillText('L', cx - r * .72, cy - r * .72);
    g.fillText('R', cx + r * .72, cy - r * .72);
    g.textAlign = 'left';
    g.fillText('OUT OF PHASE', cx - r, cy + 14);
    g.textAlign = 'right';
    g.fillText('OUT OF PHASE', cx + r, cy + 14);
  }), 0, 0, w, h);
  if (!measured)
    return;
  c.globalAlpha = .16;
  c.fillStyle = dim;
  for (let i = 0; i < measured.length; i += 4)
    if (measured[i] !== 0 || measured[i + 1] !== 0)
      c.fillRect(cx + measured[i] * r, cy - measured[i + 1] * r, 1, 1);
  c.globalAlpha = .62;
  c.fillStyle = pink;
  for (let i = 0; i < measured.length; i += 4)
    if (measured[i + 2] !== 0 || measured[i + 3] !== 0)
      c.fillRect(cx + measured[i + 2] * r, cy - measured[i + 3] * r, 1.25,
                 1.25);
  c.globalAlpha = 1;
}
function responseDraw(data) {
  const {c, w, h} = canvasContext('response'), x0 = 30, y0 = 20, ww = w - 50,
               hh = h - 40;
  c.font = '9px monospace';
  c.strokeStyle = line;
  c.fillStyle = dim;
  for (const db of [-18, 0, 18]) {
    const y = y0 + hh * (18 - db) / 36;
    c.beginPath();
    c.moveTo(x0, y);
    c.lineTo(w - 20, y);
    c.stroke();
    c.fillText(String(db), 2, y + 3);
  }
  for (const [text, f] of [[ '60 Hz', 0 ], [ '1 kHz', .5 ], [ '16k', 1 ]])
    c.fillText(text, x0 + ww * f - 8, h - 3);
  if (data) {
    c.strokeStyle = pink;
    c.beginPath();
    for (let i = 0; i < 128; i++) {
      const x = x0 + ww * i / 127,
            y = y0 + hh * (18 - clamp(data[i * 3], -18, 18)) / 36;
      i ? c.lineTo(x, y) : c.moveTo(x, y);
    }
    c.stroke();
    if (state.space_echo > 0) {
      c.strokeStyle = green;
      c.beginPath();
      for (let i = 0; i < 128; i++) {
        const x = x0 + ww * i / 127, y = y0 + hh * (1 - data[i * 3 + 1] / 2);
        i ? c.lineTo(x, y) : c.moveTo(x, y);
      }
      c.stroke();
    }
  }
  c.fillStyle = dim;
  c.fillText('dB', 2, 10);
  c.textAlign = 'right';
  if (state.space_echo > 0) {
    c.fillStyle = green;
    c.fillText('2 s', w - 1, 10);
  }
  c.textAlign = 'left';
}
function draw(now) {
  if (core) {
    let corners, response;
    if (node && liveCorners) {
      corners = liveCorners;
      response = liveResponse;
    } else {
      core.ht_visual(0);
      corners = new Float32Array(core.memory.buffer, core.ht_buffer(8), 12);
      response = new Float32Array(core.memory.buffer, core.ht_buffer(9), 384);
      activeFrame = clamp(state.text_frame - 1, 0, previews.length - 1);
    }
    if (changed('plane', `${corners.join()}|${activeFrame}|${sourceVersion}|${
                             coarsePointer.matches}`))
      planeDraw(corners);
    if (changed('response', `${state.space_echo > 0}|${response.join()}`))
      responseDraw(response);
  } else if (changed('plane', 'none'))
    planeDraw();
  if (changed('scope', measured ? scopeVersion : 'none'))
    scopeDraw();
  if (node && snapshotBuffers.length &&
      (!lastSnapshot || now - lastSnapshot > 1000)) {
    lastSnapshot = now;
    const buffer = snapshotBuffers.pop();
    node.port.postMessage({type : 'snapshot', buffer}, [ buffer ]);
  }
  setTimeout(() => requestAnimationFrame(draw), 40);
}
const pointers = new Map();
let gesture;
function localPoint(e) {
  const r = $('plane').getBoundingClientRect();
  return {x : e.clientX - r.left, y : e.clientY - r.top};
}
$('plane').onpointerdown = e => {
  const p = localPoint(e);
  pointers.set(e.pointerId, p);
  $('plane').setPointerCapture(e.pointerId);
  if (pointers.size === 2) {
    const [a, b] = [...pointers.values() ];
    gesture = {
      mode : 'pinch',
      distance : Math.hypot(a.x - b.x, a.y - b.y),
      base : {...state}
    };
    return;
  }
  let mode = e.shiftKey ? 'rotate' : e.altKey ? 'roll' : 'move';
  for (const key of ['rotate', 'scale', 'roll'])
    if (handles[key] &&
        Math.hypot(p.x - handles[key].x, p.y - handles[key].y) < 22)
      mode = key;
  gesture = {mode, p, base : {...state}};
};
$('plane').onpointermove = e => {
  if (!pointers.has(e.pointerId) || !gesture)
    return;
  const p = localPoint(e);
  pointers.set(e.pointerId, p);
  const g = gesture, dx = p.x - (g.p?.x || 0), dy = p.y - (g.p?.y || 0),
        b = g.base;
  if (g.mode === 'pinch' && pointers.size === 2) {
    const [a, z] = [...pointers.values() ],
              ratio =
                  Math.hypot(a.x - z.x, a.y - z.y) / Math.max(1, g.distance);
    state.scale_x = clamp(b.scale_x * ratio, .05, 4);
    state.scale_y = clamp(b.scale_y * ratio, .05, 4);
  } else if (g.mode === 'move') {
    state.position_x = clamp(b.position_x + dx / (handles.unit || 100), -1, 1);
    state.position_y = clamp(b.position_y - dy / (handles.unit || 100), -1, 1);
  } else if (g.mode === 'rotate') {
    state.yaw = clamp(b.yaw + dx, -180, 180);
    state.pitch = clamp(b.pitch - dy, -180, 180);
  } else if (g.mode === 'roll')
    state.roll = clamp(b.roll - dx, -180, 180);
  else if (g.mode === 'scale') {
    const ratio = Math.exp((dx + dy) / 150);
    state.scale_x = clamp(b.scale_x * ratio, .05, 4);
    state.scale_y = clamp(b.scale_y * ratio, .05, 4);
  }
  sendState();
  for (const key of axisKeys)
    refreshControl(key);
};
for (const type of ['pointerup', 'pointercancel'])
  $('plane').addEventListener(type, e => {
    pointers.delete(e.pointerId);
    gesture = null;
  });
$('plane').ondblclick = () => $('reset-plane').click();
$('shape-kind').selectedIndex = sourceMeta.shape;
refreshAll();
requestAnimationFrame(draw);
setup().catch(error => status(error.message, true));
