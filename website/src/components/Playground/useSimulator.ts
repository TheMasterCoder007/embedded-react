import {useEffect, useRef, useState, type RefObject} from 'react';

/** The engine's host ABI (backends/web/web_backend.h), as cwrapped functions. */
type Engine = {
  init: (w: number, h: number) => number;
  loadSource: (ptr: number, len: number) => void;
  loadPack: (ptr: number, len: number) => number;
  resize: (w: number, h: number) => number;
  pump: (dtMs: number) => void;
  touch: (phase: number, x: number, y: number) => void;
  framebuffer: () => number;
  fbWidth: () => number;
  fbHeight: () => number;
  module: {
    _malloc: (n: number) => number;
    _free: (p: number) => void;
    HEAPU8: Uint8Array;
    cwrap: (name: string, ret: string | null, args: string[]) => (...a: number[]) => number;
  };
};

declare global {
  interface Window {
    createEmbeddedReact?: (opts: {locateFile: (f: string) => string}) => Promise<Engine['module']>;
  }
}

// One engine per page: the wasm module is created once and every caller shares it. Coming back to
// the page reuses the running engine (resized) rather than bringing up a second one.
let enginePromise: Promise<Engine> | null = null;
let booted = false;

function loadEngine(base: string): Promise<Engine> {
  if (enginePromise) return enginePromise;
  enginePromise = new Promise<void>((ok, fail) => {
    const s = document.createElement('script');
    s.src = `${base}engine/embedded-react.js`;
    s.onload = () => ok();
    s.onerror = () => fail(new Error('could not load the engine script'));
    document.head.appendChild(s);
  })
    .then(() => window.createEmbeddedReact!({locateFile: (f) => `${base}engine/${f}`}))
    .then((module) => ({
      module,
      init: module.cwrap('er_web_init', 'number', ['number', 'number']),
      loadSource: module.cwrap('er_web_load_source', null, ['number', 'number']),
      loadPack: module.cwrap('er_web_load_pack', 'number', ['number', 'number']),
      resize: module.cwrap('er_web_resize', 'number', ['number', 'number']),
      pump: module.cwrap('er_web_pump', null, ['number']),
      touch: module.cwrap('er_web_touch', null, ['number', 'number', 'number']),
      framebuffer: module.cwrap('er_web_framebuffer', 'number', []),
      fbWidth: module.cwrap('er_web_fb_width', 'number', []),
      fbHeight: module.cwrap('er_web_fb_height', 'number', []),
    }));
  return enginePromise;
}

const withBytes = (engine: Engine, bytes: Uint8Array, fn: (ptr: number, len: number) => unknown) => {
  const ptr = engine.module._malloc(bytes.length);
  engine.module.HEAPU8.set(bytes, ptr);
  try {
    return fn(ptr, bytes.length);
  } finally {
    engine.module._free(ptr);
  }
};

export type Simulator = {
  /** Runs an app bundle, replacing the current one. */
  load: (source: string, pack: Uint8Array | null) => void;
  /** Changes the panel size; the engine re-runs the current bundle at the new size. */
  resize: (w: number, h: number) => void;
};

/**
 * Drives the engine into a canvas: brings it up at the given size, pumps it every frame with the
 * elapsed wall-clock time (the way a device host does), copies the framebuffer out, and turns
 * pointer events into touches. Mirrors the host page the simulator ships with.
 */
export function useSimulator(
  canvasRef: RefObject<HTMLCanvasElement | null>,
  base: string,
  width: number,
  height: number,
): {sim: Simulator | null; error: string | null} {
  const [sim, setSim] = useState<Simulator | null>(null);
  const [error, setError] = useState<string | null>(null);
  const size = useRef({width, height});
  size.current = {width, height};

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    let stopped = false;
    let raf = 0;
    let interval = 0;
    const cleanups: (() => void)[] = [];

    loadEngine(base)
      .then((engine) => {
        if (stopped) return;
        const ctx = canvas.getContext('2d')!;
        let w = 0;
        let h = 0;
        let image: ImageData | null = null;
        const syncCanvas = () => {
          w = engine.fbWidth();
          h = engine.fbHeight();
          canvas.width = w;
          canvas.height = h;
          image = ctx.createImageData(w, h);
        };
        const up = booted
          ? engine.resize(size.current.width, size.current.height) || 1
          : engine.init(size.current.width, size.current.height);
        if (!up) {
          setError('the engine failed to start');
          return;
        }
        booted = true;
        syncCanvas();

        // The clock is elapsed wall-clock time, however long the gap: a tab coming back from the
        // background is immediately at the right time. A slow interval keeps the app's timers
        // moving while requestAnimationFrame is throttled or stopped.
        let prev = performance.now();
        let lastFrame = prev;
        const step = (paint: boolean) => {
          const now = performance.now();
          engine.pump(Math.round(Math.min(2147483647, now - prev)));
          prev = now;
          if (!paint || !image) return;
          image.data.set(new Uint8ClampedArray(engine.module.HEAPU8.buffer, engine.framebuffer(), w * h * 4));
          ctx.putImageData(image, 0, 0);
        };
        const frame = () => {
          step(true);
          lastFrame = performance.now();
          raf = requestAnimationFrame(frame);
        };
        raf = requestAnimationFrame(frame);
        interval = window.setInterval(() => {
          if (performance.now() - lastFrame >= 250) step(!document.hidden);
        }, 250);

        // Pointer to framebuffer coordinates, through whatever CSS scaling the canvas has.
        let down = false;
        const toFb = (e: PointerEvent): [number, number] => {
          const r = canvas.getBoundingClientRect();
          return [Math.round(((e.clientX - r.left) / r.width) * w), Math.round(((e.clientY - r.top) / r.height) * h)];
        };
        const onDown = (e: PointerEvent) => {
          e.preventDefault();
          down = true;
          canvas.setPointerCapture(e.pointerId);
          engine.touch(0, ...toFb(e));
        };
        const onMove = (e: PointerEvent) => {
          if (down) engine.touch(1, ...toFb(e));
        };
        const onUp = (e: PointerEvent) => {
          if (!down) return;
          down = false;
          engine.touch(2, ...toFb(e));
        };
        canvas.addEventListener('pointerdown', onDown);
        canvas.addEventListener('pointermove', onMove);
        canvas.addEventListener('pointerup', onUp);
        canvas.addEventListener('pointercancel', onUp);
        cleanups.push(() => {
          canvas.removeEventListener('pointerdown', onDown);
          canvas.removeEventListener('pointermove', onMove);
          canvas.removeEventListener('pointerup', onUp);
          canvas.removeEventListener('pointercancel', onUp);
        });

        setSim({
          load: (source, pack) => {
            if (pack) withBytes(engine, pack, engine.loadPack);
            withBytes(engine, new TextEncoder().encode(source), engine.loadSource);
          },
          resize: (nw, nh) => {
            if (nw === w && nh === h) return;
            if (engine.resize(nw, nh)) syncCanvas();
          },
        });
      })
      .catch((e: Error) => setError(e.message));

    return () => {
      stopped = true;
      cancelAnimationFrame(raf);
      clearInterval(interval);
      cleanups.forEach((fn) => fn());
    };
    // The engine is brought up once; size changes go through `resize`.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [canvasRef, base]);

  return {sim, error};
}
