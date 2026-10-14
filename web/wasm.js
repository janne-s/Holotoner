// A standalone module; no Emscripten JavaScript runtime or Node dependencies.
export function instantiate(module) {
  const fail = () => {
    throw new Error('The audio engine exhausted its fixed memory budget.');
  };
  const instance = new WebAssembly.Instance(module, {
    env : {emscripten_notify_memory_growth : fail},
    wasi_snapshot_preview1 : {
      proc_exit : fail,
      fd_write : () => 0,
      fd_close : () => 0,
      fd_seek : () => 0
    }
  });
  instance.exports._initialize?.();
  return instance.exports;
}
export const keys = [
  'position_x',
  'position_y',
  'scale_x',
  'scale_y',
  'pitch',
  'yaw',
  'roll',
  'image_projection',
  'image_amount',
  'image_edge',
  'image_rest',
  'wet',
  'out_delta',
  'space_interfere',
  'space_echo',
  'space_feedback',
  'space_resonate',
  'image_channel',
  'image_gain',
  'image_fill',
  'image_normalize',
  'image_invert',
  'image_threshold',
  'image_binary',
  'tempo',
  'space_echo_time',
  'space_echo_time_sync',
  'motion_shape',
  'motion_depth',
  ...['pitch', 'yaw', 'roll', 'position_x', 'position_y', 'scale_x', 'scale_y']
      .flatMap(k => [k + '_rate', k + '_rate_sync']),
  'text_frame',
  'text_play',
  'text_rate',
  'text_rate_sync',
  'image_smooth'
];
export const notes = [
  'off',   '1/256', '1/128', '1/64T', '1/64', '1/32T', '1/32', '1/32D',
  '1/16T', '1/16',  '1/16D', '1/8T',  '1/8',  '1/8D',  '1/4T', '1/4',
  '1/4D',  '1/2T',  '1/2',   '1/2D',  '1/1',  '2/1',   '4/1',  '8/1'
];

// Packed analyser packet, allocated by the UI and transferred back for reuse.
export const snapshotLayout = [
  [ 'scope', 2, 16384 ], [ 'metrics', 3, 4 ], [ 'corners', 8, 12 ],
  [ 'response', 9, 384 ], [ 'plane', 10, 7 ]
];
export const snapshotByteLength =
    snapshotLayout.reduce((bytes, [, , count ]) => bytes + count * 4, 0);
