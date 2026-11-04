import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react';
import clsx from 'clsx';
import useBaseUrl from '@docusaurus/useBaseUrl';
import {useColorMode} from '@docusaurus/theme-common';

import Editor from './Editor';
import {compile, CompileError, type Files} from './compile';
import SizeDock, {clampSize, type SizeMode} from './SizeDock';
import {useSimulator} from './useSimulator';

import styles from './Playground.module.css';

// The starter that `npm create embedded-react` scaffolds.
const DEMO = 'starter';
const ENTRY_FILE = 'App.jsx';

type App = {
  entry: string;
  files: Files;
  original: Files;
  pack: Uint8Array | null;
  /** True until the demo's first load, which is not debounced. */
  fresh: boolean;
  /** Bumped by Reset so the app starts from a fresh state rather than what edits kept alive. */
  generation: number;
};

const DEBOUNCE_MS = 300;
// How long fit mode waits for the pane to stop resizing.
const FIT_DEBOUNCE_MS = 60;
// How close to whole the device pixel ratio must be for the canvas to be drawn pixelated.
const WHOLE_RATIO_TOLERANCE = 0.005;

// The engine and its persisted-state store outlive this component, so every visit and every Reset
// must start from fresh state: the store is cleared when the engine can, and the generation folded
// into the state keys advances regardless, which covers engines without the clear export.
let nextGeneration = 0;

/**
 * Edit the starter's source and see it run: the files are compiled in the browser and handed to
 * the engine, which re-evaluates them in place, the same hot reload the dev server does.
 */
export default function Playground(): ReactNode {
  const base = useBaseUrl('/playground/');
  const dark = useColorMode().colorMode === 'dark';

  const [app, setApp] = useState<App | null>(null);
  const [open, setOpen] = useState('App.jsx');
  // Like the simulator: the engine fills the pane by default, or runs at a locked panel size shown
  // 1:1, scrolling when the panel is bigger than the pane.
  const [mode, setMode] = useState<SizeMode>({kind: 'fit'});
  const [framed, setFramed] = useState(false);
  const [size, setSize] = useState({width: 480, height: 320});
  const [vendor, setVendor] = useState<string | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [compileError, setCompileError] = useState<CompileError | null>(null);

  const canvas = useRef<HTMLCanvasElement>(null);
  const {sim, error: simError} = useSimulator(
    canvas,
    base,
    size.width,
    size.height,
  );
  const stage = useRef<HTMLDivElement>(null);

  useEffect(() => {
    fetch(`${base}vendor.js`)
      .then(response =>
        response.ok
          ? response.text()
          : Promise.reject(new Error(`vendor.js: ${response.status}`)),
      )
      .then(setVendor, (error: Error) => setLoadError(error.message));
  }, [base]);

  useEffect(() => {
    setLoadError(null);
    Promise.all([
      fetch(`${base}${DEMO}/files.json`).then(response =>
        response.ok
          ? (response.json() as Promise<{entry: string; files: Files}>)
          : Promise.reject(new Error(`${DEMO}: ${response.status}`)),
      ),
      fetch(`${base}${DEMO}/assets.pack`).then(response =>
        response.ok ? response.arrayBuffer() : null,
      ),
    ]).then(
      ([demo, pack]) => {
        setApp({
          entry: demo.entry,
          files: demo.files,
          original: demo.files,
          pack: pack ? new Uint8Array(pack) : null,
          fresh: true,
          generation: nextGeneration++,
        });
        setOpen(demo.files[ENTRY_FILE] ? ENTRY_FILE : demo.entry);
      },
      (error: Error) => setLoadError(error.message),
    );
  }, [base]);

  // Compile and run on every change, debounced while typing; a freshly picked demo runs at once.
  useEffect(() => {
    if (!app || !sim || !vendor) return undefined;
    const run = () => {
      try {
        sim.load(
          compile(app.entry, app.files, vendor, app.generation),
          app.fresh ? app.pack : null,
        );
        setCompileError(null);
      } catch (error) {
        if (error instanceof CompileError) setCompileError(error);
        else throw error;
      }
    };
    if (app.fresh) {
      sim.clearPersist?.();
      run();
      return undefined;
    }
    const timer = setTimeout(run, DEBOUNCE_MS);
    return () => clearTimeout(timer);
  }, [app, sim, vendor]);

  useEffect(() => sim?.resize(size.width, size.height), [sim, size]);

  const edit = useCallback((path: string, value: string) => {
    setApp(current =>
      current && current.files[path] !== value
        ? {...current, files: {...current.files, [path]: value}, fresh: false}
        : current,
    );
  }, []);
  const reset = () => {
    sim?.clearPersist?.();
    setApp(current =>
      current
        ? {
            ...current,
            files: current.original,
            fresh: false,
            generation: nextGeneration++,
          }
        : current,
    );
  };
  const dirty = app
    ? Object.keys(app.files).some(
        path => app.files[path] !== app.original[path],
      )
    : false;

  // In fit mode the engine runs at the pane's size, re-measured (briefly debounced) as it changes.
  useEffect(() => {
    const element = stage.current;
    if (!element) return undefined;
    if (mode.kind === 'fixed') {
      setSize({width: mode.width, height: mode.height});
      return undefined;
    }
    let timer = 0;
    const fit = () => {
      clearTimeout(timer);
      timer = window.setTimeout(
        () =>
          setSize({
            width: clampSize(element.clientWidth),
            height: clampSize(element.clientHeight),
          }),
        FIT_DEBOUNCE_MS,
      );
    };
    fit();
    const observer = new ResizeObserver(fit);
    observer.observe(element);
    return () => {
      clearTimeout(timer);
      observer.disconnect();
    };
  }, [mode]);
  // Shown 1:1, so the engine's pixels line up with device pixels on an integer-ratio display.
  const pixelRatio =
    typeof window === 'undefined' ? 1 : window.devicePixelRatio;
  const exact =
    Math.abs(pixelRatio - Math.round(pixelRatio)) < WHOLE_RATIO_TOLERANCE;

  const paths = useMemo(
    () => (app ? Object.keys(app.files).sort() : []),
    [app],
  );
  const status = loadError ?? simError;

  return (
    <div className={styles.playground}>
      <div className={styles.split}>
        <section className={styles.code} aria-label="Code">
          <div className={styles.tabs} role="tablist" aria-label="Files">
            {paths.map(path => (
              <button
                key={path}
                type="button"
                role="tab"
                aria-selected={path === open}
                className={clsx(styles.tab, path === open && styles.activeTab)}
                onClick={() => setOpen(path)}>
                {path}
              </button>
            ))}
            <button
              type="button"
              className={styles.reset}
              onClick={reset}
              disabled={!dirty}>
              Reset
            </button>
          </div>
          <div className={styles.editorHost}>
            {app && app.files[open] !== undefined && (
              <Editor
                path={open}
                value={app.files[open]}
                dark={dark}
                onChange={edit}
              />
            )}
          </div>
          {compileError && (
            <div className={styles.problem} role="alert">
              <strong>
                {compileError.file}
                {compileError.line ? `:${compileError.line}` : ''}
                {compileError.column ? `:${compileError.column}` : ''}
              </strong>{' '}
              {compileError.message}
            </div>
          )}
        </section>

        <section className={styles.preview} aria-label="Preview">
          <div
            className={clsx(
              styles.stage,
              mode.kind === 'fit' ? styles.fit : styles.fixed,
            )}
            ref={stage}>
            <div
              className={clsx(
                styles.device,
                framed && mode.kind === 'fixed' && styles.framed,
              )}>
              <canvas
                ref={canvas}
                className={styles.canvas}
                style={{imageRendering: exact ? 'pixelated' : 'auto'}}
                aria-label={`The starter app running in the simulator at ${size.width} by ${size.height}`}
              />
            </div>
            {status && <p className={styles.status}>{status}</p>}
            {!status && !sim && (
              <p className={styles.status}>Loading the engine…</p>
            )}
            <SizeDock
              mode={mode}
              actual={size}
              framed={framed}
              onMode={setMode}
              onFramed={setFramed}
            />
          </div>
        </section>
      </div>
    </div>
  );
}
