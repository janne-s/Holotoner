// Optional local Chrome integration check. No third-party packages.
// Launch Chrome with --headless=new --mute-audio --remote-debugging-port=9236.
// Serve App on port 8765. The test also mutes its output connection below.
import assert from 'node:assert/strict';
import {writeFile,mkdtemp,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const browser=process.env.HOLOTONER_CDP_URL||'http://127.0.0.1:9236';
const site=process.env.HOLOTONER_TEST_URL||'http://127.0.0.1:8765/web/';
const directory=await mkdtemp(join(tmpdir(),'holotoner-web-test-'));
const target=await(await fetch(browser+'/json/new?about:blank',{method:'PUT'})).json();
const socket=new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
let next=0;const pending=new Map(),errors=[];
socket.addEventListener('message',({data})=>{const message=JSON.parse(data);if(message.id){const request=pending.get(message.id);pending.delete(message.id);message.error?request.reject(Error(message.error.message)):request.resolve(message.result);}else if(message.method==='Runtime.exceptionThrown')errors.push(message.params.exceptionDetails);});
const send=(method,params={})=>new Promise((resolve,reject)=>{const id=++next;pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));});
async function evaluate(expression){const r=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception?.description||r.exceptionDetails.text);return r.result.value;}
async function wait(expression){const start=Date.now();while(Date.now()-start<15000){if(await evaluate(expression))return;await sleep(100);}throw Error('Timed out: '+expression);}
try{
 await send('Runtime.enable');await send('Page.enable');await send('Emulation.setDeviceMetricsOverride',{width:1440,height:1000,deviceScaleFactor:1,mobile:false});
 await send('Page.addScriptToEvaluateOnNewDocument',{source:`
// Keep the worklet measurements intact while silencing the physical destination.
const connect=AudioNode.prototype.connect;
AudioNode.prototype.connect=function(destination,...args){
 if(destination instanceof AudioDestinationNode){
  const mute=this.context.createGain();mute.gain.value=0;
  connect.call(mute,destination);return connect.call(this,mute,...args);
 }
 return connect.call(this,destination,...args);
};
window.__downloads=[];const create=URL.createObjectURL;URL.createObjectURL=function(blob){window.__downloads.push(blob);return create.call(this,blob);};`});
 await send('Page.navigate',{url:site});await wait('document.querySelector("#play") && !document.querySelector("#play").disabled');
 await evaluate('document.querySelector("#save").click()');await wait('window.__downloads.length>0');
 const initial=await evaluate('window.__downloads.at(-1).text().then(JSON.parse)');
 assert.equal(initial.source.shape,7);assert.equal(initial.source.name,'Smiley');
 assert.equal(await evaluate('document.querySelector("#shape-kind").value'),'Smiley');
 const iconCentre=await evaluate(`(()=>{const b=document.querySelector('#restart').getBoundingClientRect(),i=document.querySelector('#restart svg').getBoundingClientRect();return{x:i.x+i.width/2-(b.x+b.width/2),y:i.y+i.height/2-(b.y+b.height/2)};})()`);
 assert.ok(Math.abs(iconCentre.x)<.01&&Math.abs(iconCentre.y)<.01);
 await evaluate(`document.querySelector('#motion-depth').value=.8;document.querySelector('#motion-depth').dispatchEvent(new Event('change'));document.querySelector('#motion-depth').dispatchEvent(new MouseEvent('dblclick'));`);
 assert.equal(await evaluate('document.querySelector("#motion-depth").value'),'0.5');
 await evaluate(`document.querySelector('#image_smooth').value=.001;document.querySelector('#image_smooth').dispatchEvent(new Event('input'));document.querySelector('#image_smooth').dispatchEvent(new MouseEvent('dblclick'));`);
 assert.equal(await evaluate('document.querySelector("#image_smooth").value'),'0');
 const smoothPoint=await evaluate(`(()=>{const r=document.querySelector('#image_smooth').getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2};})()`);
 await send('Input.dispatchMouseEvent',{type:'mousePressed',...smoothPoint,button:'left',clickCount:1});
 await send('Input.dispatchMouseEvent',{type:'mouseMoved',x:smoothPoint.x,y:smoothPoint.y-18,buttons:1});
 await send('Input.dispatchMouseEvent',{type:'mouseReleased',x:smoothPoint.x,y:smoothPoint.y-18,button:'left',clickCount:1});
 assert.equal(Number(await evaluate('document.querySelector("#image_smooth").value')),.00005,'Smooth lacks fine control near zero');

 await evaluate(`document.querySelector('#image_smooth').value=.0005;document.querySelector('#image_smooth').dispatchEvent(new Event('input'));`);
 assert.equal(await evaluate('document.querySelector("#image_smooth").closest(".knob").querySelector("output").textContent'),'0.50 ms');
 const playWidth=await evaluate('document.querySelector("#play").getBoundingClientRect().width');
 assert.equal(await evaluate('document.querySelector("#echo-note").hidden'),true);
 assert.equal(await evaluate('document.querySelector("#echo-sync").parentNode===document.querySelector("#space_echo_time").parentNode'),true);
 await evaluate('document.querySelector("#play").click()');await wait('document.querySelector("#play").textContent.includes("Pause")');await sleep(600);
 assert.equal(await evaluate('document.querySelector("#play").getBoundingClientRect().width'),playWidth);
 await evaluate('document.querySelector("#play").click()');await wait('document.querySelector("#play").textContent.includes("Play")');
 assert.equal(await evaluate('document.querySelector("#play").getBoundingClientRect().width'),playWidth);
 await evaluate('document.querySelector("#play").click()');await wait('document.querySelector("#play").textContent.includes("Pause")');await sleep(200);
 // Time's note replaces its knob and value, and retains the free time.
 await evaluate(`document.querySelector('#space_echo_time').value=.37;document.querySelector('#space_echo_time').dispatchEvent(new Event('input'));document.querySelector('#echo-sync').click();document.querySelector('#echo-note').value='15';document.querySelector('#echo-note').dispatchEvent(new Event('change'));`);
 assert.equal(await evaluate('document.querySelector("#space_echo_time").hidden'),true);
 assert.equal(await evaluate('document.querySelector("#space_echo_time").closest(".knob").querySelector("output").hidden'),true);
 assert.equal(await evaluate('document.querySelector("#echo-note").hidden'),false);
 await evaluate('document.querySelector("#echo-sync").click()');
 assert.equal(await evaluate('document.querySelector("#space_echo_time").value'),'0.37');
 assert.equal(await evaluate('document.querySelector("#echo-note").hidden'),true);
 await evaluate('document.querySelector("#echo-sync").click()');
 // All seven base sliders are empty at neutral. Rotation fills in either direction.
 const neutral=await evaluate(`['pitch','yaw','roll','position_x','position_y','scale_x','scale_y'].every(key=>{const s=document.querySelector('#'+key).style;return s.getPropertyValue('--fill-start')===s.getPropertyValue('--fill-end');})`);
 assert.ok(neutral);
 await evaluate(`document.querySelector('#yaw').value=-90;document.querySelector('#yaw').dispatchEvent(new Event('input'));`);
 assert.equal(await evaluate('document.querySelector("#yaw").style.getPropertyValue("--fill-start")'),'25%');
 assert.equal(await evaluate('document.querySelector("#yaw").style.getPropertyValue("--fill-end")'),'50%');
 await evaluate('document.querySelector("#reset-plane").click()');
 assert.ok(Math.abs(Number(await evaluate('document.querySelector("#correlation").value')))>0);
 await evaluate(`document.querySelector('#motion-toggle').click();document.querySelector('#yaw').value=90;document.querySelector('#yaw').dispatchEvent(new Event('input'));document.querySelector('#image_amount').value=0;document.querySelector('#image_amount').dispatchEvent(new Event('input'));`);await sleep(900);
 assert.ok(Number(await evaluate('document.querySelector("#correlation").value'))>.999);
 await evaluate(`document.querySelector('#reset-plane').click();document.querySelector('#image_amount').value=1;document.querySelector('#image_amount').dispatchEvent(new Event('input'));document.querySelector('#text-toggle').click();document.querySelector('#text').value='👩🏽‍🚀 A';document.querySelector('#text-layout').value='Characters';document.querySelector('#text-form').requestSubmit();`);await wait('document.querySelector("#status").textContent.includes("2 frame(s)")');
 await evaluate('document.querySelector("#save").click()');await wait('window.__downloads.length>0');const state=await evaluate('window.__downloads.at(-1).text().then(JSON.parse)');assert.equal(state.version,1);assert.equal(state.parameters.image_smooth,.0005);assert.equal(state.frames.length,2);assert.equal(state.parameters.motion_depth,.5);assert.equal(state.parameters.space_echo_time_sync,15);assert.equal(state.parameters.space_echo_time,.37);
 const statePath=join(directory,'state.json');await writeFile(statePath,JSON.stringify(state));
 await evaluate('document.querySelector("#shape-toggle").click()');await wait('document.querySelector("#status").textContent.includes("Shape loaded")');
 await evaluate(`document.querySelector('#status').textContent='';document.querySelector('#shape-kind').selectedIndex=2;document.querySelector('#shape-kind').dispatchEvent(new Event('change'));`);await wait('document.querySelector("#status").textContent.includes("Shape loaded")');
 await evaluate('document.querySelector("#save").click()');await sleep(100);const rings=await evaluate('window.__downloads.at(-1).text().then(JSON.parse)');assert.equal(rings.source.kind,'shape');assert.equal(rings.source.shape,2);
 await evaluate(`document.querySelector('#status').textContent='';document.querySelector('#shape-order').value=5;document.querySelector('#shape-order').dispatchEvent(new Event('input'));`);await wait('document.querySelector("#status").textContent.includes("Shape loaded")');
 await evaluate('document.querySelector("#save").click()');await sleep(100);const ordered=await evaluate('window.__downloads.at(-1).text().then(JSON.parse)');assert.equal(ordered.source.order,5);assert.notEqual(ordered.frames[0].rgba,rings.frames[0].rgba);
 assert.equal(await evaluate('document.querySelector("#shape-panel").textContent.includes("Use shape")'),false);
 await evaluate(`document.querySelector('#status').textContent='';document.querySelector('#shape-kind').value='Smiley';document.querySelector('#shape-kind').dispatchEvent(new Event('change'));`);await wait('document.querySelector("#status").textContent.includes("Shape loaded")');
 await evaluate('document.querySelector("#save").click()');await sleep(100);const smiley=await evaluate('window.__downloads.at(-1).text().then(JSON.parse)');assert.equal(smiley.source.shape,7);assert.equal(smiley.source.name,'Smiley');
 const smileyPath=join(directory,'smiley.json');await writeFile(smileyPath,JSON.stringify(smiley));
 const {root}=await send('DOM.getDocument');const {nodeId}=await send('DOM.querySelector',{nodeId:root.nodeId,selector:'#state-file'});await send('DOM.setFileInputFiles',{nodeId,files:[statePath]});await wait('document.querySelector("#status").textContent.includes("State restored")');await evaluate('document.querySelector("#save").click()');await sleep(100);const restored=await evaluate('window.__downloads.at(-1).text().then(JSON.parse)');assert.equal(restored.parameters.image_smooth,.0005);assert.equal(restored.source.text,'👩🏽‍🚀 A');assert.equal(restored.parameters.space_echo_time_sync,15);assert.equal(await evaluate('document.querySelector("#space_echo_time").hidden'),true);
 await send('DOM.setFileInputFiles',{nodeId,files:[smileyPath]});await wait('document.querySelector("#status").textContent.includes("State restored")');await evaluate('document.querySelector("#save").click()');await sleep(100);const reopenedSmiley=await evaluate('window.__downloads.at(-1).text().then(JSON.parse)');assert.deepEqual(reopenedSmiley.source,smiley.source);assert.deepEqual(reopenedSmiley.frames,smiley.frames);
 const legacy={...state,version:1,parameters:{...state.parameters}};delete legacy.parameters.image_smooth;
 const legacyPath=join(directory,'legacy.json');await writeFile(legacyPath,JSON.stringify(legacy));
 await evaluate('document.querySelector("#status").textContent=""');
 await send('DOM.setFileInputFiles',{nodeId,files:[legacyPath]});await wait('document.querySelector("#status").textContent.includes("State restored")');
 assert.equal(await evaluate('document.querySelector("#image_smooth").value'),'0');
 state.parameters.yaw=999;const invalidPath=join(directory,'invalid.json');await writeFile(invalidPath,JSON.stringify(state));await send('DOM.setFileInputFiles',{nodeId,files:[invalidPath]});await wait('document.querySelector("#status").textContent.includes("out of range")');assert.equal(await evaluate('document.querySelector("#yaw").value'),'0');

 // Local media and the direct plane gesture must update the same base controls.
 const audioNode=await send('DOM.querySelector',{nodeId:root.nodeId,selector:'#audio-file'});
 await send('DOM.setFileInputFiles',{nodeId:audioNode.nodeId,files:[fileURLToPath(new URL('../assets/demo.wav',import.meta.url))]});
 await wait('document.querySelector("#status").textContent.includes("Audio loaded")');
 assert.equal(await evaluate('document.querySelector("#audio-name").textContent'),'demo.wav');
 const centre=await evaluate(`(()=>{const r=document.querySelector('#plane').getBoundingClientRect();return{x:r.left+r.width/2,y:r.top+r.height/2};})()`);
 await send('Input.dispatchMouseEvent',{type:'mousePressed',...centre,button:'left',clickCount:1});
 await send('Input.dispatchMouseEvent',{type:'mouseMoved',x:centre.x+20,y:centre.y,buttons:1});
 await send('Input.dispatchMouseEvent',{type:'mouseReleased',x:centre.x+20,y:centre.y,button:'left',clickCount:1});
 assert.ok(Number(await evaluate('document.querySelector("#position_x").value'))>0);
 await evaluate('document.querySelector("#reset-plane").click()');
 // A local PNG is decoded without an upload and saved with its original aspect.
 const png=await evaluate(`(()=>{const canvas=document.createElement('canvas');canvas.width=90;canvas.height=45;const c=canvas.getContext('2d');c.fillStyle='white';c.fillRect(10,5,70,35);return canvas.toDataURL('image/png').split(',')[1];})()`);
 const imagePath=join(directory,'image.png');await writeFile(imagePath,Buffer.from(png,'base64'));
 const imageNode=await send('DOM.querySelector',{nodeId:root.nodeId,selector:'#image-file'});await send('DOM.setFileInputFiles',{nodeId:imageNode.nodeId,files:[imagePath]});await wait('document.querySelector("#status").textContent.includes("Image loaded")');
 await evaluate('document.querySelector("#save").click()');await sleep(100);const saved=await evaluate('window.__downloads.at(-1).text().then(JSON.parse)');assert.equal(saved.frames[0].width,90);assert.equal(saved.frames[0].height,45);

 // Observe the real canvas drawing without adding a test API to the app.
 await evaluate(`
 if(document.querySelector('#play').textContent.includes('Pause'))document.querySelector('#play').click();
 for(const source of ['text','shape'])if(!document.querySelector('#'+source+'-panel').hidden)document.querySelector('#'+source+'-toggle').click();
 window.__planeHandles={};
 const planeContext=document.querySelector('#plane').getContext('2d');
 const moveTo=planeContext.moveTo,rect=planeContext.rect,arc=planeContext.arc;
 planeContext.moveTo=function(x,y){if(this.strokeStyle==='#3ccf9b')window.__planeHandles.rotate={x,y:y+6};return moveTo.call(this,x,y);};
 planeContext.rect=function(x,y,w,h){if(w===10&&h===10)window.__planeHandles.scale={x:x+5,y:y+5};return rect.call(this,x,y,w,h);};
 planeContext.arc=function(x,y,...args){window.__planeHandles.roll={x,y};return arc.call(this,x,y,...args);};`);
 async function verifyClampedHandles(touch){
  const pose=async(x=0,y=0,roll=0)=>{
   await evaluate(`document.querySelector('#reset-plane').click();for(const [key,value]of Object.entries({scale_x:4,scale_y:4,position_x:${x},position_y:${y},roll:${roll}})){const input=document.querySelector('#'+key);input.value=value;input.dispatchEvent(new Event('input'));}`);
   await sleep(150);
  };
  for(const [x,y,roll]of [[0,0,0],[1,1,45],[-1,-1,-45]]){
   await pose(x,y,roll);
   const drawn=await evaluate(`({points:window.__planeHandles,bounds:document.querySelector('#plane').getBoundingClientRect().toJSON()})`);
   assert.equal(Object.keys(drawn.points).length,3);
   for(const [name,p]of Object.entries(drawn.points)){
    assert.ok(p.x>=8&&p.x<=drawn.bounds.width-8&&p.y>=8&&p.y<=drawn.bounds.height-8,`off-panel ${name} handle`);
   }
  }
  for(const mode of ['rotate','scale','roll']){
   await pose();
   const p=await evaluate(`(()=>{const h=window.__planeHandles.${mode},r=document.querySelector('#plane').getBoundingClientRect();return{x:r.x+h.x,y:r.y+h.y};})()`);
   const target=await evaluate(`document.elementFromPoint(${p.x},${p.y})?.outerHTML.slice(0,200)`);
   if(touch){
    const point={...p,id:1,radiusX:6,radiusY:6,force:1};
    await send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[point]});
    await send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{...point,x:p.x-20,y:p.y+10}]});
    await send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
    await sleep(100);
   }else{
    await send('Input.dispatchMouseEvent',{type:'mousePressed',...p,button:'left',clickCount:1});
    await send('Input.dispatchMouseEvent',{type:'mouseMoved',x:p.x-20,y:p.y+10,button:'left',buttons:1});
    await send('Input.dispatchMouseEvent',{type:'mouseReleased',x:p.x-20,y:p.y+10,button:'left',clickCount:1});
   }
   const values=await evaluate(`Object.fromEntries(['yaw','pitch','roll','scale_x','scale_y','position_x','position_y'].map(k=>[k,Number(document.querySelector('#'+k).value)]))`);
   assert.equal(values.position_x,0,`${mode} handle started a move`);assert.equal(values.position_y,0);
   if(mode==='rotate'){assert.equal(values.yaw,-20);assert.equal(values.pitch,-10);assert.equal(values.scale_x,4);}
   if(mode==='roll'){assert.equal(values.roll,20);assert.equal(values.yaw,0);assert.equal(values.scale_x,4);}
   if(mode==='scale'){assert.ok(values.scale_x<4&&values.scale_x>3,JSON.stringify({mode,touch,p,target,values}));assert.equal(values.yaw,0);assert.equal(values.roll,0);}
  }
  // At a normal scale, the drawn roll handle must move with the horizontal
  // drag. This checks the visible projection, not just the angle's sign.
  for(const dx of [-15,15]){
   await pose();
   await evaluate(`for(const key of ['scale_x','scale_y']){const input=document.querySelector('#'+key);input.value=.6;input.dispatchEvent(new Event('input'));}`);
   await sleep(150);
   const before=await evaluate(`({...window.__planeHandles.roll,bounds:document.querySelector('#plane').getBoundingClientRect().toJSON()})`);
   const p={x:before.bounds.x+before.x,y:before.bounds.y+before.y};
   if(touch){
    const point={...p,id:1,radiusX:6,radiusY:6,force:1};
    await send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[point]});
    await send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{...point,x:p.x+dx}]});
    await send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
   }else{
    await send('Input.dispatchMouseEvent',{type:'mousePressed',...p,button:'left',clickCount:1});
    await send('Input.dispatchMouseEvent',{type:'mouseMoved',x:p.x+dx,y:p.y,button:'left',buttons:1});
    await send('Input.dispatchMouseEvent',{type:'mouseReleased',x:p.x+dx,y:p.y,button:'left',clickCount:1});
   }
   await sleep(150);
   const after=await evaluate('window.__planeHandles.roll');
   assert.ok((after.x-before.x)*dx>0,'Drawn roll handle moves opposite the drag');
  }
  await evaluate('document.querySelector("#reset-plane").click()');
 }
 await verifyClampedHandles(false);
 for(const [width,height,portrait]of [[390,844,true],[844,390,false]]){
  await send('Emulation.setTouchEmulationEnabled',{enabled:true,maxTouchPoints:2});
  await send('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:true});await sleep(250);
  const layout=await evaluate(`({scroll:document.documentElement.scrollWidth,left:document.querySelector('.plane-panel').getBoundingClientRect().toJSON(),right:document.querySelector('.audio-panel').getBoundingClientRect().toJSON()})`);
  assert.ok(layout.scroll<=width);assert.ok(portrait?layout.right.top>layout.left.top:layout.right.left>layout.left.left);
  await verifyClampedHandles(true);
 }
 assert.equal(errors.length,0,JSON.stringify(errors));console.log('Browser checks passed: default Smiley, visible roll drag direction, Smooth control/reset/state and legacy migration, playback, mono, Unicode frames, state restore/rejection, local media, plane gestures and clamped edge handles (mouse/touch), automatic Shape activation, Smiley state, auto Shape/Order, Depth reset, centred restart icon, Time sync, neutral slider fills, fixed transport width, portrait and landscape.');
}finally{await send('Page.close');socket.close();await rm(directory,{recursive:true,force:true});}
