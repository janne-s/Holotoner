import {instantiate} from './wasm.js';
let core;
onmessage = ({data}) => {
  try {
    if (data.type === 'init') {
      core = instantiate(data.module);
      postMessage({type : 'ready'});
      return;
    }
    new Float32Array(core.memory.buffer, core.ht_buffer(4), 48).set(data.params);
    let images = data.images;
    if (data.shape) {
      core.ht_shape(data.shape.kind, data.shape.order);
      images = [ {
        width : 256,
        height : 256,
        pixels : new Uint8Array(core.memory.buffer, core.ht_buffer(6), 256 * 256 * 4).slice()
      } ];
    }
    const size = core.ht_mask_size(), masks = new Uint8Array(size * images.length), previews = [];
    for (let i = 0; i < images.length; i++) {
      const image = images[i];
      new Uint8Array(core.memory.buffer, core.ht_buffer(6), image.pixels.length).set(image.pixels);
      core.ht_compile(image.width, image.height);
      masks.set(new Uint8Array(core.memory.buffer, core.ht_buffer(7), size), i * size);
      previews.push(new Float32Array(core.memory.buffer, core.ht_buffer(7), 4096).slice());
    }
    postMessage({type : 'masks', id : data.id, masks, previews, images},
                [ masks.buffer, ...previews.map(p => p.buffer) ]);
  } catch (error) {
    postMessage({type : 'error', id : data.id, message : error.message});
  }
};
