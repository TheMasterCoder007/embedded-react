import type {ReactNode} from 'react';
import Heading from '@theme/Heading';

import CommandLine from '../CommandLine/CommandLine';
import CtaLink, {CtaRow} from '../CtaLink/CtaLink';
import NavyBand from '../NavyBand/NavyBand';
import {CREATE_COMMAND, REPO_URL} from '../content';

import styles from './Closing.module.css';

export default function Closing(): ReactNode {
  return (
    <NavyBand className={styles.closing}>
      <Heading as="h2" className={styles.title}>
        Write React. Flash it.
      </Heading>
      <CommandLine text={CREATE_COMMAND} />
      <CtaRow className={styles.buttons}>
        <CtaLink to="/getting-started">Get started</CtaLink>
        <CtaLink href={REPO_URL} variant="ghost">
          View on GitHub
        </CtaLink>
      </CtaRow>
    </NavyBand>
  );
}
