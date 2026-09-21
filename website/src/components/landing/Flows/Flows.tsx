import type {ReactNode} from 'react';
import clsx from 'clsx';
import Link from '@docusaurus/Link';
import Heading from '@theme/Heading';

import Section from '../Section/Section';
import card from '../card.module.css';

import styles from './Flows.module.css';

const flows = [
  {
    tag: 'Flow A · runtime',
    title: 'React on QuickJS',
    steps: ['JSX', 'esbuild bundle', 'QuickJS bytecode', 'React reconciler on the chip', 'C engine'],
    body: 'Full runtime dynamism: live state, anything JavaScript can express, and hot reload. Wants a chip with PSRAM, such as the ESP32-S3.',
  },
  {
    tag: 'Flow B · ahead of time',
    title: 'JSX compiled to C',
    steps: ['JSX', 'AOT compiler', 'Generated C', 'Linked into firmware', 'C engine'],
    body: 'State, handlers and animations are baked into C at build time. Smaller, deterministic, and at home on MCUs with no PSRAM.',
  },
];

export default function Flows(): ReactNode {
  return (
    <Section eyebrow="Two ways to ship" title="The same app, resolved at runtime or at compile time." tinted>
      <div className={styles.flows}>
        {flows.map((f) => (
          <article key={f.tag} className={clsx(card.card, styles.flow)}>
            <p className={styles.tag}>{f.tag}</p>
            <Heading as="h3">{f.title}</Heading>
            <ol className={styles.steps}>
              {f.steps.map((s) => (
                <li key={s}>{s}</li>
              ))}
            </ol>
            <p className={styles.body}>{f.body}</p>
          </article>
        ))}
      </div>
      <p className={styles.more}>
        <Link to="/concepts/two-flows">How the two flows compare →</Link>
      </p>
    </Section>
  );
}
