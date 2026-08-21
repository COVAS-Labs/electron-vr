export {};

declare global {
  interface Window {
    overlayDemo?: {
      getDiagnostics(): Promise<{
        platform: string; electron: string; chrome: string; backend: string; mode: string;
        runtime: string; hostApplication: string; graphicsApi: string; transport: string;
        connected: boolean; protocol: number; submitted: number; consumed: number;
      } | null>;
    };
  }
}

const sessionStatus = document.querySelector<HTMLElement>("#session-status");
const platformValue = document.querySelector<HTMLElement>("#platform-value");
const clockValue = document.querySelector<HTMLElement>("#clock-value");
const hostValue = document.querySelector<HTMLElement>("#host-value");
const runtimeValue = document.querySelector<HTMLElement>("#runtime-value");
const apiValue = document.querySelector<HTMLElement>("#api-value");
const transportValue = document.querySelector<HTMLElement>("#transport-value");
const protocolValue = document.querySelector<HTMLElement>("#protocol-value");
const framesValue = document.querySelector<HTMLElement>("#frames-value");

const updateDiagnostics = async () => {
  const diagnostics = await window.overlayDemo?.getDiagnostics();
  if (!diagnostics) return;
  if (sessionStatus) sessionStatus.textContent = diagnostics.connected ? "Connected" : "Waiting for host";
  if (hostValue) hostValue.textContent = diagnostics.hostApplication;
  if (platformValue) platformValue.textContent = `${diagnostics.backend.toUpperCase()} / ${diagnostics.mode}`;
  if (runtimeValue) runtimeValue.textContent = `${diagnostics.runtime} on ${diagnostics.platform} · Electron ${diagnostics.electron}`;
  if (apiValue) apiValue.textContent = diagnostics.graphicsApi.toUpperCase();
  if (transportValue) transportValue.textContent = diagnostics.transport;
  if (protocolValue) protocolValue.textContent = diagnostics.protocol ? `v${diagnostics.protocol}` : "N/A";
  if (framesValue) framesValue.textContent = `${diagnostics.submitted} submitted / ${diagnostics.consumed} consumed`;
};

const updateClock = () => {
  if (!clockValue) {
    return;
  }

  clockValue.textContent = new Intl.DateTimeFormat(undefined, {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  }).format(new Date());
};

updateClock();
void updateDiagnostics();
window.setInterval(updateClock, 1000);
window.setInterval(() => void updateDiagnostics(), 1000);
