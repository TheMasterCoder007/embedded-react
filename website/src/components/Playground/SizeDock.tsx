import {useState, type ReactNode} from 'react';
import clsx from 'clsx';

import styles from './SizeDock.module.css';

export type SizeMode = {kind: 'fit'} | {kind: 'fixed'; w: number; h: number};

/** The simulator's panel presets, plus the RP2040 watch board's. */
export const PRESETS: [number, number][] = [
  [800, 480],
  [1024, 600],
  [480, 320],
  [320, 240],
  [240, 320],
  [240, 280],
];

export const clampSize = (n: number): number =>
  Math.max(64, Math.min(4096, n | 0));

type Props = {
  mode: SizeMode;
  /** The size the engine is running at right now. */
  actual: {w: number; h: number};
  framed: boolean;
  onMode: (mode: SizeMode) => void;
  onFramed: (framed: boolean) => void;
};

/**
 * The floating size controls the simulator's own host page has: a gear that opens a dock with the
 * panel presets, a custom size, and the cosmetic device frame.
 */
export default function SizeDock({
  mode,
  actual,
  framed,
  onMode,
  onFramed,
}: Props): ReactNode {
  const [open, setOpen] = useState(false);
  const [w, setW] = useState(String(actual.w));
  const [h, setH] = useState(String(actual.h));
  const presetValue =
    mode.kind === 'fit'
      ? 'fit'
      : PRESETS.some(([pw, ph]) => pw === mode.w && ph === mode.h)
        ? `${mode.w}x${mode.h}`
        : 'custom';

  const lock = () => {
    const nw = clampSize(Number(w));
    const nh = clampSize(Number(h));
    setW(String(nw));
    setH(String(nh));
    onMode({kind: 'fixed', w: nw, h: nh});
  };

  return (
    <div className={clsx(styles.dock, !open && styles.collapsed)}>
      <button
        type="button"
        className={styles.toggle}
        title="Size controls"
        aria-label="Size controls"
        aria-expanded={open}
        onClick={() => {
          setW(String(actual.w));
          setH(String(actual.h));
          setOpen(o => !o);
        }}>
        <svg viewBox="0 0 24 24" width="16" height="16" aria-hidden="true">
          <circle cx="12" cy="12" r="3" />
          <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" />
        </svg>
      </button>
      {open && (
        <div className={styles.controls}>
          <label htmlFor="pg-size-preset">size</label>
          <select
            id="pg-size-preset"
            value={presetValue}
            onChange={e => {
              const v = e.target.value;
              if (v === 'fit') onMode({kind: 'fit'});
              else if (v !== 'custom') {
                const [pw, ph] = v.split('x').map(Number);
                setW(String(pw));
                setH(String(ph));
                onMode({kind: 'fixed', w: pw, h: ph});
              }
            }}>
            <option value="fit">Fit to window</option>
            {PRESETS.map(([pw, ph]) => (
              <option key={`${pw}x${ph}`} value={`${pw}x${ph}`}>
                {pw} × {ph}
              </option>
            ))}
            {presetValue === 'custom' && <option value="custom">Custom</option>}
          </select>
          <input
            type="number"
            min={64}
            max={4096}
            value={w}
            aria-label="Width"
            onChange={e => setW(e.target.value)}
            onKeyDown={e => e.key === 'Enter' && lock()}
          />
          <span>×</span>
          <input
            type="number"
            min={64}
            max={4096}
            value={h}
            aria-label="Height"
            onChange={e => setH(e.target.value)}
            onKeyDown={e => e.key === 'Enter' && lock()}
          />
          <button type="button" className={styles.act} onClick={lock}>
            Lock
          </button>
          <label className={styles.chk}>
            <input
              type="checkbox"
              checked={framed}
              onChange={e => onFramed(e.target.checked)}
            />
            frame
          </label>
          <span className={styles.sizeLabel}>
            {actual.w}×{actual.h}
            {mode.kind === 'fit' ? ' · fit' : ''}
          </span>
        </div>
      )}
    </div>
  );
}
