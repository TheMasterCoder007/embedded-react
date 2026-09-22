import type {ReactNode} from 'react';
import clsx from 'clsx';
import Heading from '@theme/Heading';

import Section from '../Section/Section';
import card from '../card.module.css';
import {icons, type IconName} from './icons';

import styles from './Features.module.css';

const features: {icon: IconName; title: string; body: string}[] = [
  {
    icon: 'atom',
    title: 'The React you already know',
    body: 'JSX, hooks, the Animated API and flexbox styles, exactly as you would write them for iOS or Android.',
  },
  {
    icon: 'chip',
    title: 'A pure C99 engine',
    body: 'Scene graph, Yoga flexbox layout, anti-aliased shapes, shadows, transforms and gradients, drawn straight into a framebuffer or SPI display.',
  },
  {
    icon: 'split',
    title: 'One app, two flows',
    body: 'Run a real React reconciler on QuickJS, or compile the same JSX ahead of time to C. It is a build flag, not a rewrite.',
  },
  {
    icon: 'feather',
    title: 'Fits chips with no external RAM',
    body: 'The ahead-of-time flow leaves no JavaScript engine and no garbage collector on the device, so small MCUs are in reach.',
  },
  {
    icon: 'bolt',
    title: 'Hot reload, no hardware',
    body: 'The engine also compiles to WebAssembly. Develop in the browser with a device frame, then flash the same app.',
  },
  {
    icon: 'touch',
    title: 'Native animation and touch',
    body: 'Animations, multitouch hit-testing, scroll momentum and dial widgets run inside the engine, not in JavaScript.',
  },
];

export default function Features(): ReactNode {
  return (
    <Section
      eyebrow="Why Embedded React"
      title="Not React talking to a microcontroller. React on the microcontroller.">
      <div className={styles.grid}>
        {features.map((f) => (
          <article key={f.title} className={clsx(card.card, card.interactive, styles.feature)}>
            <svg viewBox="0 0 24 24" className={styles.icon} aria-hidden="true">
              {icons[f.icon]}
            </svg>
            <Heading as="h3">{f.title}</Heading>
            <p>{f.body}</p>
          </article>
        ))}
      </div>
    </Section>
  );
}
