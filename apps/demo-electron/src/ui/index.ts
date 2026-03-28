export {};

declare global {
  interface Window {
    overlayDemo?: {
      environment?: {
        platform?: string;
        electron?: string;
        chrome?: string;
      };
    };
  }
}

const sessionStatus = document.querySelector<HTMLElement>("#session-status");
const platformValue = document.querySelector<HTMLElement>("#platform-value");
const clockValue = document.querySelector<HTMLElement>("#clock-value");

if (sessionStatus) {
  sessionStatus.textContent = "Frames streaming";
}

if (platformValue) {
  const environment = window.overlayDemo?.environment;
  const platform = environment?.platform ?? "unknown";
  const electron = environment?.electron ?? "?";
  platformValue.textContent = `${platform} / Electron ${electron}`;
}

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
window.setInterval(updateClock, 1000);
