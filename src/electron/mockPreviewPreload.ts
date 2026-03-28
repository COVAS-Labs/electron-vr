import { contextBridge, ipcRenderer } from "electron";
import * as electronRenderer from "electron/renderer";

type SharedTextureReceiver = {
  importedSharedTexture: {
    getVideoFrame(): VideoFrame;
    release(): void;
  };
};

type SharedTextureRendererModule = {
  setSharedTextureReceiver(handler: (payload: SharedTextureReceiver) => void | Promise<void>): void;
};

type PreviewMeta = {
  backend: string;
  frameCount: number;
  status: string;
};

type WebGpuNavigator = Navigator & {
  gpu?: {
    requestAdapter(): Promise<{
      requestDevice(): Promise<any>;
    } | null>;
    getPreferredCanvasFormat(): any;
  };
};

const sharedTexture = (electronRenderer as typeof electronRenderer & { sharedTexture?: SharedTextureRendererModule }).sharedTexture;

declare global {
  interface Window {
    mockPreviewBridge: {
      onMeta(handler: (meta: PreviewMeta) => void): void;
      setSharedTextureReceiver(handler: (frame: VideoFrame) => Promise<void> | void): void;
    };
  }
}

contextBridge.exposeInMainWorld("mockPreviewBridge", {
  onMeta(handler: (meta: PreviewMeta) => void) {
    ipcRenderer.on("mock-preview-meta", (_event, meta: PreviewMeta) => {
      handler(meta);
    });
  },
  setSharedTextureReceiver(handler: (frame: VideoFrame) => Promise<void> | void) {
    if (!sharedTexture) {
      throw new Error("Electron sharedTexture renderer API is unavailable in this runtime.");
    }

    sharedTexture.setSharedTextureReceiver(async ({ importedSharedTexture }) => {
      const frame = importedSharedTexture.getVideoFrame();

      try {
        await handler(frame);
      } finally {
        frame.close();
        importedSharedTexture.release();
      }
    });
  }
});

window.addEventListener("DOMContentLoaded", async () => {
  const canvas = document.querySelector<HTMLCanvasElement>("#preview-canvas");
  const backendValue = document.querySelector<HTMLElement>("#backend-value");
  const frameCountValue = document.querySelector<HTMLElement>("#frame-count-value");
  const statusValue = document.querySelector<HTMLElement>("#status-value");
  const snapshotValue = document.querySelector<HTMLElement>("#snapshot-value");

  if (!canvas || !backendValue || !frameCountValue || !statusValue || !snapshotValue) {
    return;
  }

  let framesRendered = 0;
  document.body.dataset.previewReady = "false";
  document.body.dataset.framesRendered = String(framesRendered);

  window.mockPreviewBridge.onMeta((meta) => {
    backendValue.textContent = meta.backend;
    frameCountValue.textContent = String(meta.frameCount);
    statusValue.textContent = meta.status;
  });

  const context = canvas.getContext("webgpu") as any;
  if (!context) {
    statusValue.textContent = "error: webgpu unavailable";
    return;
  }

  const gpuNavigator = navigator as WebGpuNavigator;
  if (!gpuNavigator.gpu) {
    statusValue.textContent = "error: navigator.gpu unavailable";
    return;
  }

  const adapter = await gpuNavigator.gpu.requestAdapter();
  if (!adapter) {
    statusValue.textContent = "error: no adapter";
    return;
  }

  const device = await adapter.requestDevice();
  const format = gpuNavigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: "premultiplied" });
  const fragmentVisibility = 0x10;

  const bindGroupLayout = device.createBindGroupLayout({
    entries: [
      {
        binding: 0,
        visibility: fragmentVisibility,
        externalTexture: {}
      },
      {
        binding: 1,
        visibility: fragmentVisibility,
        sampler: {}
      }
    ]
  });

  const pipeline = device.createRenderPipeline({
    layout: device.createPipelineLayout({ bindGroupLayouts: [bindGroupLayout] }),
    vertex: {
      module: device.createShaderModule({
        code: `
          @vertex
          fn main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
            var positions = array<vec2<f32>, 6>(
              vec2<f32>(-1.0, -1.0),
              vec2<f32>(1.0, -1.0),
              vec2<f32>(-1.0, 1.0),
              vec2<f32>(-1.0, 1.0),
              vec2<f32>(1.0, -1.0),
              vec2<f32>(1.0, 1.0)
            );
            return vec4<f32>(positions[index], 0.0, 1.0);
          }
        `
      }),
      entryPoint: "main"
    },
    fragment: {
      module: device.createShaderModule({
        code: `
          @group(0) @binding(0) var extTex: texture_external;
          @group(0) @binding(1) var extSampler: sampler;

          @fragment
          fn main(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
            let uv = pos.xy / vec2<f32>(1280.0, 720.0);
            return textureSampleBaseClampToEdge(extTex, extSampler, uv);
          }
        `
      }),
      entryPoint: "main",
      targets: [{ format }]
    },
    primitive: {
      topology: "triangle-list"
    }
  });

  window.mockPreviewBridge.setSharedTextureReceiver(async (frame) => {
    const externalTexture = device.importExternalTexture({ source: frame });
    const bindGroup = device.createBindGroup({
      layout: bindGroupLayout,
      entries: [
        { binding: 0, resource: externalTexture },
        { binding: 1, resource: device.createSampler() }
      ]
    });

    const commandEncoder = device.createCommandEncoder();
    const renderPass = commandEncoder.beginRenderPass({
      colorAttachments: [
        {
          view: context.getCurrentTexture().createView(),
          clearValue: { r: 0.0, g: 0.0, b: 0.0, a: 1.0 },
          loadOp: "clear",
          storeOp: "store"
        }
      ]
    });

    renderPass.setPipeline(pipeline);
    renderPass.setBindGroup(0, bindGroup);
    renderPass.draw(6);
    renderPass.end();
    device.queue.submit([commandEncoder.finish()]);
    if (typeof device.queue.onSubmittedWorkDone === "function") {
      await device.queue.onSubmittedWorkDone();
    }

    framesRendered += 1;
    document.body.dataset.previewReady = "true";
    document.body.dataset.framesRendered = String(framesRendered);
    snapshotValue.textContent = String(canvas.toDataURL("image/png").length);
  });
});

export {};
