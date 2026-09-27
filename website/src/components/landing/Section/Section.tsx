import type {ReactNode} from 'react';
import clsx from 'clsx';
import Heading from '@theme/Heading';

import styles from './Section.module.css';

type Props = {
  eyebrow: string;
  title: string;
  /** A faintly shaded background, to set a section apart from its neighbours. */
  tinted?: boolean;
  children: ReactNode;
};

/** A landing-page content section: eyebrow label, heading, then the body. Follows the color mode. */
export default function Section({eyebrow, title, tinted, children}: Props): ReactNode {
  return (
    <section className={clsx(styles.section, tinted && styles.tinted)}>
      <div className="container">
        <p className={styles.eyebrow}>{eyebrow}</p>
        <Heading as="h2" className={styles.title}>
          {title}
        </Heading>
        {children}
      </div>
    </section>
  );
}
