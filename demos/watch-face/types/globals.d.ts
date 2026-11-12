// The panel size, provided by the host: injected at runtime by Flow A, read at boot by the Flow B runtime.
declare const screen: {readonly width: number; readonly height: number};

// The web's timers, which the runtime services once per frame.
declare function setTimeout(callback: () => void, delay?: number): number;
declare function clearTimeout(id: number): void;
declare function setInterval(callback: () => void, delay?: number): number;
declare function clearInterval(id: number): void;
