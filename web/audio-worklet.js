import {instantiate, snapshotByteLength, snapshotLayout} from './wasm.js';

class HolotonerProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.core = instantiate(options.processorOptions.module);
    this.core.ht_init(sampleRate);
    const c = this.core, b = c.memory.buffer;
    this.left = new Float32Array(b, c.ht_buffer(0), 2048);
    this.right = new Float32Array(b, c.ht_buffer(1), 2048);
    this.params = new Float32Array(b, c.ht_buffer(4), 48);
    this.snapshotViews = snapshotLayout.map(
        ([ key, id,
           count ]) => [key, new Float32Array(b, c.ht_buffer(id), count)]);
    this.playing = false;
    this.bank = 0;
    this.pendingCount = -1;
    this.port.onmessage = ({data}) => {
      if (data.type === 'params')
        this.params.set(data.values);
      if (data.type === 'transport') {
        this.playing = data.playing;
        if (data.reset)
          c.ht_reset();
      }
      if (data.type === 'masks') {
        const inactive = 1 - this.bank;
        new Uint8Array(b, c.ht_buffer(5) + inactive * 32 * c.ht_mask_size(),
                       data.masks.length)
            .set(data.masks);
        this.pendingCount = data.count;
      }
      // The UI owns two preallocated packets. Fill and return one without
      // allocating a sample buffer or cloning the engine's entire WASM heap.
      if (data.type === 'snapshot' && data.buffer instanceof ArrayBuffer &&
          data.buffer.byteLength === snapshotByteLength && data.buffer !== b) {
        c.ht_snapshot();
        c.ht_visual(1);
        const snapshot = {type : 'snapshot'};
        let offset = 0;
        for (const [key, view] of this.snapshotViews) {
          snapshot[key] = new Float32Array(data.buffer, offset, view.length);
          snapshot[key].set(view);
          offset += view.byteLength;
        }
        this.port.postMessage(snapshot, [ data.buffer ]);
      }
    };
    this.port.postMessage({type : 'ready'});
  }
  process(inputs, outputs) {
    const input = inputs[0], out = outputs[0], n = out[0].length;
    if (n > 2048) {
      for (let j = 0; j < out.length; j++)
        out[j].fill(0);
      return true;
    }
    for (let i = 0; i < n; i++) {
      this.left[i] = input[0]?.[i] || 0;
      this.right[i] = (input[1] || input[0])?.[i] || 0;
    }
    if (this.pendingCount >= 0) {
      this.bank = 1 - this.bank;
      this.core.ht_commit(this.bank, this.pendingCount);
      this.pendingCount = -1;
    }
    this.core.ht_process(n, this.playing ? 1 : 0);
    for (let i = 0; i < n; i++) {
      out[0][i] = this.left[i];
      if (out[1])
        out[1][i] = this.right[i];
    }
    return true;
  }
}
registerProcessor('holotoner', HolotonerProcessor);
