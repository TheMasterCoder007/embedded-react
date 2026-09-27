import type {ReactNode} from 'react';
import clsx from 'clsx';

import styles from './NavyBand.module.css';

type Props = {
  as?: 'header' | 'section';
  className?: string;
  children: ReactNode;
};

/**
 * A full-width band that stays navy in both color modes, like the icon and the banners: the
 * gradient, plus a faint dot grid fading in towards the edges.
 */
export default function NavyBand({as: Tag = 'section', className, children}: Props): ReactNode {
  return (
    <Tag className={clsx(styles.band, className)}>
      <div className={clsx('container', styles.content)}>{children}</div>
    </Tag>
  );
}
