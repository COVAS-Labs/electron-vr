import { app } from "electron";
import { VROverlay } from "../../packages/electron-vr/dist/index.js";

app.commandLine.appendSwitch("enable-features", "SharedImages");
const html = `<!doctype html><style>html,body{margin:0;overflow:hidden}canvas{display:block}</style>
<canvas width="1280" height="720"></canvas><script>
const canvas=document.querySelector('canvas'),ctx=canvas.getContext('2d',{alpha:false});let frame=0;
function draw(){
 ctx.fillStyle='#ff0000';ctx.fillRect(0,0,640,360);
 ctx.fillStyle='#00ff00';ctx.fillRect(640,0,640,360);
 ctx.fillStyle='#0000ff';ctx.fillRect(0,360,640,360);
 ctx.fillStyle='#ffff00';ctx.fillRect(640,360,640,360);
 ctx.fillStyle='rgb('+(frame%251)+','+((frame*7)%251)+','+((frame*13)%251)+')';ctx.fillRect(500,250,280,220);
 ctx.fillStyle='#ffffff';ctx.font='32px monospace';ctx.fillText(String(frame++),540,360);
 requestAnimationFrame(draw);
}draw();</script>`;
let overlay;
app.whenReady().then(async () => {
  overlay = new VROverlay({ name: "Direct GPU pattern validation", width: 1280, height: 720,
    sizeMeters: 1, url: `data:text/html;charset=utf-8,${encodeURIComponent(html)}` });
  if (!await overlay.init()) { console.error("Failed to initialize VR bridge"); app.exit(1); }
  await app.getGPUInfo("basic");
  if (app.getGPUFeatureStatus().gpu_compositing !== "enabled") {
    console.error("GPU compositing is not enabled:", app.getGPUFeatureStatus().gpu_compositing);
    app.exit(1);
  }
}).catch((error) => { console.error(error); app.exit(1); });
app.on("before-quit", () => overlay?.destroy());
app.on("window-all-closed", () => app.quit());
