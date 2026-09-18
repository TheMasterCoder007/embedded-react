import type {ReactNode} from 'react';
import clsx from 'clsx';
import '@fontsource-variable/jetbrains-mono/wght.css';

import styles from './styles.module.css';

/**
 * The project name as a wordmark: one word, the weight and color change standing in for the
 * hyphen. Plain text (page titles, prose) says "Embedded React"; code keeps `embedded-react`.
 */
export default function Wordmark({className}: {className?: string}): ReactNode {
  return (
    <span className={clsx(styles.wordmark, className)} role="img" aria-label="Embedded React">
      <span className={styles.embedded}>embedded</span>
      <span className={styles.react}>React</span>
    </span>
  );
}
