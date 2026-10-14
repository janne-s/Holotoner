// Build-time checks only. Uses Node built-ins; the shipped application does not.
import { readFile } from 'node:fs/promises';
import assert from 'node:assert/strict';
import { performance } from 'node:perf_hooks';
const source=await readFile(new URL('../wasm.js',import.meta.url),'utf8');
const {instantiate,keys,snapshotByteLength}=await import('data:text/javascript;base64,'+Buffer.from(source).toString('base64'));
const module=await WebAssembly.compile(await readFile(new URL('../assets/holotoner.wasm',import.meta.url)));
function prepare(sr){const c=instantiate(module);c.ht_init(sr);const p=new Float32Array(c.memory.buffer,c.ht_buffer(4),48);p[2]=p[3]=1;p[8]=p[9]=p[11]=1;p[17]=1;p[18]=p[20]=1;p[24]=120;p[25]=.25;p[28]=.5;p[43]=1;p[45]=.25;return {c,p,l:new Float32Array(c.memory.buffer,c.ht_buffer(0),2048),r:new Float32Array(c.memory.buffer,c.ht_buffer(1),2048)};}
function mask(c,kind=3){c.ht_shape(kind,3);c.ht_compile(256,256);const m=new Uint8Array(c.memory.buffer,c.ht_buffer(7),c.ht_mask_size()).slice();new Uint8Array(c.memory.buffer,c.ht_buffer(5),m.length).set(m);c.ht_commit(0,1);return m;}
assert.equal(keys.length,48);
for(const sr of [44100,48000,96000]){
 for(const block of [1,31,128,512]){
  const {c,p,l,r}=prepare(sr);p[11]=0;mask(c);c.ht_process(1,0);c.ht_reset();const count=8192,output=[];
  for(let at=0;at<count;at+=block){const n=Math.min(block,count-at);for(let i=0;i<n;i++){l[i]=Math.sin((at+i)*.13)*.1;r[i]=Math.cos((at+i)*.17)*.08;}c.ht_process(n,1);for(let i=0;i<n;i++)output.push([l[i],r[i]]);}
  for(let i=0;i<count;i++){const t=i-2047;assert.ok(Math.abs(output[i][0]-(t<0?0:Math.sin(t*.13)*.1))<2e-7,`aligned Dry ${sr}/${block}`);assert.ok(Number.isFinite(output[i][1]));}
 }
 const {c,p,l,r}=prepare(sr);const m=mask(c);c.ht_shape(3,3);c.ht_compile(256,256);assert.deepEqual(new Uint8Array(c.memory.buffer,c.ht_buffer(7),m.length),m,'deterministic mask');
 const bytes=c.memory.buffer.byteLength;p[8]=0;p[5]=90;c.ht_process(1,0);c.ht_reset();
 let ll=0,rr=0,lr=0;
 for(let at=0;at<sr*.25;at+=128){for(let i=0;i<128;i++){l[i]=Math.sin((at+i)*.1)*.1;r[i]=Math.cos((at+i)*.07)*.1;}c.ht_process(128,1);if(at>sr*.1)for(let i=0;i<128;i++){ll+=l[i]*l[i];rr+=r[i]*r[i];lr+=l[i]*r[i];}}
 assert.ok(lr/Math.sqrt(ll*rr)>.9999,'90-degree mono');
 // All effects with continuous motion and a prepared mask must remain finite and bounded.
 p[8]=1;p[13]=p[14]=p[15]=p[16]=1;p[31]=.3;p[27]=1;
 const start=performance.now();
 for(let at=0;at<sr*2;at+=128){for(let i=0;i<128;i++){l[i]=Math.sin((at+i)*.06)*.08;r[i]=l[i];}c.ht_process(128,1);for(let i=0;i<128;i++){assert.ok(Number.isFinite(l[i])&&Number.isFinite(r[i]));assert.ok(Math.abs(l[i])<=4&&Math.abs(r[i])<=4);}}
 const elapsed=performance.now()-start;
 assert.equal(c.memory.buffer.byteLength,bytes,'fixed WASM memory');
 c.ht_snapshot();c.ht_visual(1);assert.ok(new Float32Array(c.memory.buffer,c.ht_buffer(2),16384).every(Number.isFinite));assert.ok(new Float32Array(c.memory.buffer,c.ht_buffer(9),384).every(Number.isFinite));
 console.log(`${sr} Hz: Dry blocks 1/31/128/512, masks, mono, motion/effects passed; 2 s all-effects audio in ${elapsed.toFixed(1)} ms`);
}
// Transport clock partition independence and pause holds.
function motion(block,pause=false){const {c,p}=prepare(48000);p[31]=.25;p[27]=1;for(let i=0;i<48000;i+=block)c.ht_process(Math.min(block,48000-i),1);c.ht_visual(1);const before=new Float32Array(c.memory.buffer,c.ht_buffer(8),12).slice();if(pause){for(let i=0;i<100;i++)c.ht_process(128,0);c.ht_visual(1);const held=new Float32Array(c.memory.buffer,c.ht_buffer(8),12).slice();for(let i=0;i<100;i++)c.ht_process(128,0);c.ht_visual(1);assert.deepEqual(new Float32Array(c.memory.buffer,c.ht_buffer(8),12),held);}return before;}
const a=motion(1),b=motion(31),d=motion(512);for(let i=0;i<12;i++){assert.ok(Math.abs(a[i]-b[i])<1e-6);assert.ok(Math.abs(a[i]-d[i])<1e-6);}motion(128,true);
console.log('Audio-owned motion is independent of block partition; pause holds.');
// Exercise the actual worklet adapter with its prepared WASM, including rapid
// updates and all 32 frame slots. Snapshot messages must stay out of process().
let Processor;
globalThis.sampleRate=48000;
globalThis.AudioWorkletProcessor=class{constructor(){this.port={postMessage(){}};}};
globalThis.registerProcessor=(name,type)=>{assert.equal(name,'holotoner');Processor=type;};
const helpers='data:text/javascript;base64,'+Buffer.from(source).toString('base64');
const adapter=(await readFile(new URL('../audio-worklet.js',import.meta.url),'utf8')).replace("'./wasm.js'",JSON.stringify(helpers));
await import('data:text/javascript;base64,'+Buffer.from(adapter).toString('base64'));
const worklet=new Processor({processorOptions:{module}});
const params=new Float32Array(48);params[2]=params[3]=params[8]=params[9]=params[11]=params[17]=params[18]=params[20]=1;params[24]=120;params[25]=.25;params[28]=.5;params[43]=1;params[45]=.25;
worklet.port.onmessage({data:{type:'params',values:params}});
const {c}=prepare(48000),m=mask(c),bank=new Uint8Array(m.length*32);for(let i=0;i<32;i++)bank.set(m,i*m.length);
for(let i=0;i<3;i++)worklet.port.onmessage({data:{type:'masks',masks:bank,count:32}});
const input=new Float32Array(128),output=[new Float32Array(128),new Float32Array(128)];for(let i=0;i<128;i++)input[i]=Math.sin(i*.1)*.1;
worklet.port.postMessage=()=>{assert.fail('Message allocation inside process()');};
assert.equal(worklet.process([[input]],[output]),true);assert.equal(worklet.pendingCount,-1);assert.equal(worklet.bank,1);
assert.ok(output.flatMap(ch=>Array.from(ch)).every(Number.isFinite));
let snapshot;worklet.port.postMessage=value=>snapshot=value;worklet.port.onmessage({data:{type:'snapshot',buffer:new ArrayBuffer(snapshotByteLength)}});assert.equal(snapshot.scope.length,16384);assert.equal(snapshot.plane.length,7);
console.log('Actual worklet adapter: mono input, 32 frames, rapid bank updates and off-render snapshots passed.');
// Text advances on the sample clock, with the same note convention as Motion.
const text=prepare(48000),textMask=mask(text.c),twoFrames=new Uint8Array(textMask.length*2);twoFrames.set(textMask);twoFrames.set(textMask,textMask.length);new Uint8Array(text.c.memory.buffer,text.c.ht_buffer(5),twoFrames.length).set(twoFrames);text.c.ht_commit(0,2);text.p[44]=1;text.p[45]=2;text.p[46]=15; // quarter note at 120 BPM = 0.5 seconds
for(let i=0;i<36000;i+=128)text.c.ht_process(Math.min(128,36000-i),1);
text.c.ht_snapshot();assert.equal(new Float32Array(text.c.memory.buffer,text.c.ht_buffer(3),4)[2],1);
for(let i=0;i<100;i++)text.c.ht_process(128,0);text.c.ht_snapshot();assert.equal(new Float32Array(text.c.memory.buffer,text.c.ht_buffer(3),4)[2],1);
console.log('Note-synced text selection and transport hold passed.');

// Echo sync replaces free Time at the native note value and current tempo.
const echo=prepare(48000);echo.p[25]=.37;echo.p[26]=0;echo.c.ht_visual(0);
const times=()=>new Float32Array(echo.c.memory.buffer,echo.c.ht_buffer(9),384);
const free=times()[64*3+1];echo.p[26]=15;echo.c.ht_visual(0);const quarter=times()[64*3+1];
assert.ok(Math.abs(quarter/free-.5/.37)<1e-6);echo.p[24]=60;echo.c.ht_visual(0);assert.ok(Math.abs(times()[64*3+1]/quarter-2)<1e-6);
echo.p[26]=0;echo.c.ht_visual(0);assert.equal(times()[64*3+1],free);
console.log('Echo Time: free milliseconds, quarter note, tempo change and return to free passed.');

// An analyser update must not clone or transfer the whole WASM heap. Exercise
// actual transfer semantics, then compare audio with an unpolled processor.
const polled=new Processor({processorOptions:{module}}),unpolled=new Processor({processorOptions:{module}});
for(const instance of [polled,unpolled]){
 instance.port.onmessage({data:{type:'params',values:params}});
 instance.port.onmessage({data:{type:'masks',masks:bank,count:32}});
}
let payload;
polled.port.postMessage=(message,transfer)=>payload=structuredClone(message,{transfer});
const polledOut=[new Float32Array(128),new Float32Array(128)],unpolledOut=[new Float32Array(128),new Float32Array(128)];
for(let block=0;block<100;block++){
 for(let i=0;i<128;i++)input[i]=Math.sin((block*128+i)*.07)*.1;
 polled.process([[input]],[polledOut]);unpolled.process([[input]],[unpolledOut]);
 assert.deepEqual(polledOut,unpolledOut,'scope polling changes audio');
 if(block%5===0){
  const packet=structuredClone(payload?.scope.buffer || new ArrayBuffer(snapshotByteLength),
      {transfer:payload ? [payload.scope.buffer] : []});
  polled.port.onmessage({data:{type:'snapshot',buffer:packet}});
  const buffers=new Set(Object.values(payload).filter(v=>ArrayBuffer.isView(v)).map(v=>v.buffer));
  assert.equal(buffers.size,1,'scope packet must have one reusable buffer');
  assert.equal([...buffers][0].byteLength,snapshotByteLength,'scope message includes WASM heap');
  assert.equal(polled.core.memory.buffer.byteLength,32*1024*1024,'scope transfer detached engine memory');
  assert.equal(payload.scope.length,16384);
 }
}
console.log('Scope transfers recycle one 67,164-byte packet and do not change audio or detach WASM memory.');

// Smooth must reach the shared core, retain sample energy, and use the sample
// clock rather than the render quantum. Defaults remain the unsmoothed path.
function renderSmooth(seconds,block,sr=48000){
 const {c,p,l,r}=prepare(sr);mask(c);p[47]=seconds;
 c.ht_process(1,0);c.ht_reset();const count=32768,out=[new Float32Array(count),new Float32Array(count)];
 for(let at=0;at<count;at+=block){const n=Math.min(block,count-at);
  for(let i=0;i<n;i++){l[i]=Math.sin((at+i)*.13)*.2;r[i]=Math.cos((at+i)*.077)*.17;}
  c.ht_process(n,1);out[0].set(l.subarray(0,n),at);out[1].set(r.subarray(0,n),at);
 }
 return out;
}
for(const sr of [44100,48000,96000]){
 const hard=renderSmooth(0,512,sr),smooth=renderSmooth(.001,512,sr);
 assert.notDeepEqual(hard,smooth,'Smooth parameter never reached the engine');
 for(let i=0;i<hard[0].length;i++){
  const power=a=>a[0][i]**2+a[1][i]**2;
  assert.ok(Number.isFinite(power(smooth))&&Math.abs(power(hard)-power(smooth))<3e-8,'Smooth changed sample energy');
 }
 for(const block of [1,31])assert.deepEqual(renderSmooth(.001,block,sr),smooth,'Smooth depends on block partition');
}
console.log('Smooth bridge: sample energy, changed steering and exact partitions at 44.1/48/96 kHz passed.');
