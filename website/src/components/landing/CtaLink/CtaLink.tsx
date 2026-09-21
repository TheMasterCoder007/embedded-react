import type {ReactNode} from 'react';
import clsx from 'clsx';
import Link from '@docusaurus/Link';

import styles from './CtaLink.module.css';

type Props = {
  to?: string;
  href?: string;
  variant?: 'primary' | 'ghost';
  children: ReactNode;
};

/** A large call-to-action button, colored for a navy band. */
export default function CtaLink({variant = 'primary', children, ...target}: Props): ReactNode {
  return (
    <Link className={clsx('button button--lg', styles[variant])} {...target}>
      {children}
    </Link>
  );
}

/** Lays out a row of CtaLinks. */
export function CtaRow({className, children}: {className?: string; children: ReactNode}): ReactNode {
  return <div className={clsx(styles.row, className)}>{children}</div>;
}
