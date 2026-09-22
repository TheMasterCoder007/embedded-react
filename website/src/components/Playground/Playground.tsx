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

// Persisted state is keyed by generation; the engine and its store outlive this component, so
// every visit (and every Reset) takes a new one and starts from fresh state.
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
  const [size, setSize] = useState({w: 480, h: 320});
  const [vendor, setVendor] = useState<string | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [compileError, setCompileError] = useState<CompileError | null>(null);

  const canvas = useRef<HTMLCanvasElement>(null);
  const {sim, error: simError} = useSimulator(canvas, base, size.w, size.h);
  const stage = useRef<HTMLDivElement>(null);

  useEffect(() => {
    fetch(`${base}vendor.js`)
      .then(r =>
        r.ok ? r.text() : Promise.reject(new Error(`vendor.js: ${r.status}`)),
      )
      .then(setVendor, (e: Error) => setLoadError(e.message));
  }, [base]);

  useEffect(() => {
    setLoadError(null);
    Promise.all([
      fetch(`${base}${DEMO}/files.json`).then(r =>
        r.ok
          ? (r.json() as Promise<{entry: string; files: Files}>)
          : Promise.reject(new Error(`${DEMO}: ${r.status}`)),
      ),
      fetch(`${base}${DEMO}/assets.pack`).then(r =>
        r.ok ? r.arrayBuffer() : null,
      ),
    ]).then(
      ([src, pack]) => {
        setApp({
          entry: src.entry,
          files: src.files,
          original: src.files,
          pack: pack ? new Uint8Array(pack) : null,
          fresh: true,
          generation: nextGeneration++,
        });
        setOpen(src.files[ENTRY_FILE] ? ENTRY_FILE : src.entry);
      },
      (e: Error) => setLoadError(e.message),
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
      } catch (e) {
        if (e instanceof CompileError) setCompileError(e);
        else throw e;
      }
    };
    if (app.fresh) {
      run();
      return undefined;
    }
    const t = setTimeout(run, DEBOUNCE_MS);
    return () => clearTimeout(t);
  }, [app, sim, vendor]);

  useEffect(() => sim?.resize(size.w, size.h), [sim, size]);

  const edit = useCallback((path: string, value: string) => {
    setApp(a =>
      a && a.files[path] !== value
        ? {...a, files: {...a.files, [path]: value}, fresh: false}
        : a,
    );
  }, []);
  const reset = () =>
    setApp(a =>
      a
        ? {...a, files: a.original, fresh: false, generation: nextGeneration++}
        : a,
    );
  const dirty = app
    ? Object.keys(app.files).some(k => app.files[k] !== app.original[k])
    : false;

  // In fit mode the engine runs at the pane's size, re-measured (briefly debounced) as it changes.
  useEffect(() => {
    const el = stage.current;
    if (!el) return undefined;
    if (mode.kind === 'fixed') {
      setSize({w: mode.w, h: mode.h});
      return undefined;
    }
    let t = 0;
    const fit = () => {
      clearTimeout(t);
      t = window.setTimeout(
        () =>
          setSize({
            w: clampSize(el.clientWidth),
            h: clampSize(el.clientHeight),
          }),
        60,
      );
    };
    fit();
    const ro = new ResizeObserver(fit);
    ro.observe(el);
    return () => {
      clearTimeout(t);
      ro.disconnect();
    };
  }, [mode]);
  // Shown 1:1, so the engine's pixels line up with device pixels on an integer-ratio display.
  const dpr = typeof window === 'undefined' ? 1 : window.devicePixelRatio;
  const exact = Math.abs(dpr - Math.round(dpr)) < 0.005;

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
            {paths.map(p => (
              <button
                key={p}
                type="button"
                role="tab"
                aria-selected={p === open}
                className={clsx(styles.tab, p === open && styles.activeTab)}
                onClick={() => setOpen(p)}>
                {p}
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
                aria-label={`The starter app running in the simulator at ${size.w} by ${size.h}`}
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
