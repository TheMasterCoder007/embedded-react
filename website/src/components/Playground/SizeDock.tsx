import {useState, type ReactNode} from 'react';
import clsx from 'clsx';

import styles from './SizeDock.module.css';

export type SizeMode =
  {kind: 'fit'} | {kind: 'fixed'; width: number; height: number};

/** The simulator's panel presets, plus the RP2040 watch board's. */
export const PRESETS: [number, number][] = [
  [800, 480],
  [1024, 600],
  [480, 320],
  [320, 240],
  [240, 320],
  [240, 280],
];

const MIN_SIZE = 64;
const MAX_SIZE = 4096;

export const clampSize = (size: number): number =>
  Math.max(MIN_SIZE, Math.min(MAX_SIZE, size | 0));

type Props = {
  mode: SizeMode;
  /** The size the engine is running at right now. */
  actual: {width: number; height: number};
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
  const [widthText, setWidthText] = useState(String(actual.width));
  const [heightText, setHeightText] = useState(String(actual.height));
  const presetValue =
    mode.kind === 'fit'
      ? 'fit'
      : PRESETS.some(
            ([presetWidth, presetHeight]) =>
              presetWidth === mode.width && presetHeight === mode.height,
          )
        ? `${mode.width}x${mode.height}`
        : 'custom';

  const lock = () => {
    const lockedWidth = clampSize(Number(widthText));
    const lockedHeight = clampSize(Number(heightText));
    setWidthText(String(lockedWidth));
    setHeightText(String(lockedHeight));
    onMode({kind: 'fixed', width: lockedWidth, height: lockedHeight});
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
          setWidthText(String(actual.width));
          setHeightText(String(actual.height));
          setOpen(wasOpen => !wasOpen);
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
            onChange={event => {
              const value = event.target.value;
              if (value === 'fit') onMode({kind: 'fit'});
              else if (value !== 'custom') {
                const [presetWidth, presetHeight] = value
                  .split('x')
                  .map(Number);
                setWidthText(String(presetWidth));
                setHeightText(String(presetHeight));
                onMode({
                  kind: 'fixed',
                  width: presetWidth,
                  height: presetHeight,
                });
              }
            }}>
            <option value="fit">Fit to window</option>
            {PRESETS.map(([presetWidth, presetHeight]) => (
              <option
                key={`${presetWidth}x${presetHeight}`}
                value={`${presetWidth}x${presetHeight}`}>
                {presetWidth} × {presetHeight}
              </option>
            ))}
            {presetValue === 'custom' && <option value="custom">Custom</option>}
          </select>
          <input
            type="number"
            min={MIN_SIZE}
            max={MAX_SIZE}
            value={widthText}
            aria-label="Width"
            onChange={event => setWidthText(event.target.value)}
            onKeyDown={event => event.key === 'Enter' && lock()}
          />
          <span>×</span>
          <input
            type="number"
            min={MIN_SIZE}
            max={MAX_SIZE}
            value={heightText}
            aria-label="Height"
            onChange={event => setHeightText(event.target.value)}
            onKeyDown={event => event.key === 'Enter' && lock()}
          />
          <button type="button" className={styles.act} onClick={lock}>
            Lock
          </button>
          <label className={styles.chk}>
            <input
              type="checkbox"
              checked={framed}
              onChange={event => onFramed(event.target.checked)}
            />
            frame
          </label>
          <span className={styles.sizeLabel}>
            {actual.width}×{actual.height}
            {mode.kind === 'fit' ? ' · fit' : ''}
          </span>
        </div>
      )}
    </div>
  );
}
