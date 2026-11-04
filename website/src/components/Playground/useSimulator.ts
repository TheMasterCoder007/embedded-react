import {useEffect, useRef, useState, type RefObject} from 'react';

/** The engine's host ABI (backends/web/web_backend.h), as cwrapped functions. */
type Engine = {
  init: (width: number, height: number) => number;
  loadSource: (pointer: number, length: number) => void;
  loadPack: (pointer: number, length: number) => number;
  resize: (width: number, height: number) => number;
  pump: (elapsedMs: number) => void;
  touch: (phase: number, x: number, y: number) => void;
  framebuffer: () => number;
  framebufferWidth: () => number;
  framebufferHeight: () => number;
  /** Forgets persisted state; absent in engines older than the export (then the caller works around it). */
  clearPersist: (() => void) | null;
  module: {
    _malloc: (size: number) => number;
    _free: (pointer: number) => void;
    HEAPU8: Uint8Array;
    cwrap: (
      name: string,
      returnType: string | null,
      argumentTypes: string[],
    ) => (...values: number[]) => number;
    _er_web_clear_persist?: () => void;
  };
};

declare global {
  interface Window {
    createEmbeddedReact?: (options: {
      locateFile: (file: string) => string;
    }) => Promise<Engine['module']>;
  }
}

// The engine's touch phases, as the host page passes them.
const TOUCH_DOWN = 0;
const TOUCH_MOVE = 1;
const TOUCH_UP = 2;

const BYTES_PER_PIXEL = 4;
const INT32_MAX = 2147483647;
// How often the app's timers still run while requestAnimationFrame is throttled or stopped.
const BACKGROUND_TICK_MS = 250;

// One engine per page: the wasm module is created once and every caller shares it. Coming back to
// the page reuses the running engine (resized) rather than bringing up a second one.
let enginePromise: Promise<Engine> | null = null;
let booted = false;

function loadEngine(base: string): Promise<Engine> {
  if (enginePromise) return enginePromise;
  enginePromise = new Promise<void>((resolve, reject) => {
    const script = document.createElement('script');
    script.src = `${base}engine/embedded-react.js`;
    script.onload = () => resolve();
    script.onerror = () =>
      reject(new Error('could not load the engine script'));
    document.head.appendChild(script);
  })
    .then(() =>
      window.createEmbeddedReact!({
        locateFile: file => `${base}engine/${file}`,
      }),
    )
    .then(module => ({
      module,
      init: module.cwrap('er_web_init', 'number', ['number', 'number']),
      loadSource: module.cwrap('er_web_load_source', null, [
        'number',
        'number',
      ]),
      loadPack: module.cwrap('er_web_load_pack', 'number', [
        'number',
        'number',
      ]),
      resize: module.cwrap('er_web_resize', 'number', ['number', 'number']),
      pump: module.cwrap('er_web_pump', null, ['number']),
      touch: module.cwrap('er_web_touch', null, ['number', 'number', 'number']),
      framebuffer: module.cwrap('er_web_framebuffer', 'number', []),
      framebufferWidth: module.cwrap('er_web_fb_width', 'number', []),
      framebufferHeight: module.cwrap('er_web_fb_height', 'number', []),
      clearPersist: module._er_web_clear_persist
        ? () => module._er_web_clear_persist!()
        : null,
    }));
  return enginePromise;
}

const withBytes = (
  engine: Engine,
  bytes: Uint8Array,
  callback: (pointer: number, length: number) => unknown,
) => {
  const pointer = engine.module._malloc(bytes.length);
  engine.module.HEAPU8.set(bytes, pointer);
  try {
    return callback(pointer, bytes.length);
  } finally {
    engine.module._free(pointer);
  }
};

export type Simulator = {
  /** Runs an app bundle, replacing the current one. */
  load: (source: string, pack: Uint8Array | null) => void;
  /** Changes the panel size; the engine re-runs the current bundle at the new size. */
  resize: (width: number, height: number) => void;
  /** Forgets persisted state, when the engine supports it. */
  clearPersist: (() => void) | null;
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
    let animationFrame = 0;
    let interval = 0;
    const cleanups: (() => void)[] = [];

    loadEngine(base)
      .then(engine => {
        if (stopped) return;
        const context = canvas.getContext('2d')!;
        let framebufferWidth = 0;
        let framebufferHeight = 0;
        let image: ImageData | null = null;
        const syncCanvas = () => {
          framebufferWidth = engine.framebufferWidth();
          framebufferHeight = engine.framebufferHeight();
          canvas.width = framebufferWidth;
          canvas.height = framebufferHeight;
          image = context.createImageData(framebufferWidth, framebufferHeight);
        };
        const started = booted
          ? engine.resize(size.current.width, size.current.height) || 1
          : engine.init(size.current.width, size.current.height);
        if (!started) {
          setError('the engine failed to start');
          return;
        }
        booted = true;
        syncCanvas();

        // The clock is elapsed wall-clock time, however long the gap: a tab coming back from the
        // background is immediately at the right time. A slow interval keeps the app's timers
        // moving while requestAnimationFrame is throttled or stopped.
        let previousTime = performance.now();
        let lastFrame = previousTime;
        const step = (paint: boolean) => {
          const now = performance.now();
          engine.pump(Math.round(Math.min(INT32_MAX, now - previousTime)));
          previousTime = now;
          if (!paint || !image) return;
          image.data.set(
            new Uint8ClampedArray(
              engine.module.HEAPU8.buffer,
              engine.framebuffer(),
              framebufferWidth * framebufferHeight * BYTES_PER_PIXEL,
            ),
          );
          context.putImageData(image, 0, 0);
        };
        const frame = () => {
          step(true);
          lastFrame = performance.now();
          animationFrame = requestAnimationFrame(frame);
        };
        animationFrame = requestAnimationFrame(frame);
        interval = window.setInterval(() => {
          if (performance.now() - lastFrame >= BACKGROUND_TICK_MS)
            step(!document.hidden);
        }, BACKGROUND_TICK_MS);

        // Pointer to framebuffer coordinates, through whatever CSS scaling the canvas has.
        let pointerDown = false;
        const toFb = (event: PointerEvent): [number, number] => {
          const bounds = canvas.getBoundingClientRect();
          return [
            Math.round(
              ((event.clientX - bounds.left) / bounds.width) * framebufferWidth,
            ),
            Math.round(
              ((event.clientY - bounds.top) / bounds.height) *
                framebufferHeight,
            ),
          ];
        };
        const onDown = (event: PointerEvent) => {
          event.preventDefault();
          pointerDown = true;
          canvas.setPointerCapture(event.pointerId);
          engine.touch(TOUCH_DOWN, ...toFb(event));
        };
        const onMove = (event: PointerEvent) => {
          if (pointerDown) engine.touch(TOUCH_MOVE, ...toFb(event));
        };
        const onUp = (event: PointerEvent) => {
          if (!pointerDown) return;
          pointerDown = false;
          engine.touch(TOUCH_UP, ...toFb(event));
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
            withBytes(
              engine,
              new TextEncoder().encode(source),
              engine.loadSource,
            );
          },
          resize: (newWidth, newHeight) => {
            if (
              newWidth === framebufferWidth &&
              newHeight === framebufferHeight
            )
              return;
            if (engine.resize(newWidth, newHeight)) syncCanvas();
          },
          clearPersist: engine.clearPersist,
        });
      })
      .catch((caught: Error) => setError(caught.message));

    return () => {
      stopped = true;
      cancelAnimationFrame(animationFrame);
      clearInterval(interval);
      cleanups.forEach(cleanup => cleanup());
    };
    // The engine is brought up once; size changes go through `resize`.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [canvasRef, base]);

  return {sim, error};
}
