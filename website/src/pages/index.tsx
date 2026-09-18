import type {ReactNode} from 'react';
import clsx from 'clsx';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import Heading from '@theme/Heading';
import CodeBlock from '@theme/CodeBlock';

import styles from './index.module.css';

const sample = `import {View, Text, Pressable} from 'embedded-react';

export default function App() {
  return (
    <View style={{flex: 1, padding: 20, backgroundColor: '#1a1a2e'}}>
      <Text style={{color: '#fff', fontSize: 24}}>Hello from an ESP32.</Text>
      <Pressable onPress={() => console.log('tapped')}>
        <Text style={{color: '#e94560', marginTop: 12}}>Tap me</Text>
      </Pressable>
    </View>
  );
}`;

function Hero() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <header className={clsx('hero', styles.hero)}>
      <div className="container">
        <Heading as="h1" className="hero__title">
          {siteConfig.title}
        </Heading>
        <p className="hero__subtitle">{siteConfig.tagline}</p>
        <div className={styles.buttons}>
          <Link className="button button--primary button--lg" to="/getting-started">
            Get started
          </Link>
          <Link className="button button--secondary button--lg" to="/playground">
            Try it in the browser
          </Link>
        </div>
      </div>
    </header>
  );
}

const points: {title: string; body: string; to: string}[] = [
  {
    title: 'React on the metal',
    body: 'The same JSX, Animated API and flexbox styles as React Native, rendered by a pure C99 engine straight into a framebuffer or SPI display. No OS, no browser, no phone.',
    to: '/intro',
  },
  {
    title: 'One engine, two flows',
    body: 'Run a real React reconciler on QuickJS for full runtime dynamism, or compile the same app ahead of time to C for chips with no PSRAM. The choice is a build flag, not a rewrite.',
    to: '/concepts/two-flows',
  },
  {
    title: 'ESP32, STM32, RP2040',
    body: 'Verified on hardware from an 800x480 RGB panel with PSRAM down to a 240x280 SPI display on an RP2040, plus Linux and Raspberry Pi for desktop development.',
    to: '/guides',
  },
];

export default function Home(): ReactNode {
  return (
    <Layout description="React Native for embedded MCUs. Write React, flash it, and the UI runs on the chip.">
      <Hero />
      <main className="container margin-vert--lg">
        <div className="row">
          {points.map((p) => (
            <div key={p.title} className="col col--4 margin-bottom--lg">
              <Heading as="h3">{p.title}</Heading>
              <p>{p.body}</p>
              <Link to={p.to}>Learn more</Link>
            </div>
          ))}
        </div>
        <div className="row">
          <div className="col col--8 col--offset-2">
            <CodeBlock language="jsx" title="src/App.jsx">
              {sample}
            </CodeBlock>
          </div>
        </div>
      </main>
    </Layout>
  );
}
