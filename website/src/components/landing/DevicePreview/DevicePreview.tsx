import type {ReactNode} from 'react';
import clsx from 'clsx';

import styles from './DevicePreview.module.css';

/** What the hero's sample app draws, as the panel would show it. */
export default function DevicePreview({className}: {className?: string}): ReactNode {
  return (
    <figure className={clsx(styles.device, className)}>
      <div className={styles.screen}>
        <p>Hello from an ESP32.</p>
        <p className={styles.tap}>Tap me</p>
      </div>
      <figcaption>
        <i /> running on the chip
      </figcaption>
    </figure>
  );
}
