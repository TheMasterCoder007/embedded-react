import type {ReactNode} from 'react';
import Heading from '@theme/Heading';

import Wordmark from '@site/src/components/Wordmark/Wordmark';

import CodeWindow from '../CodeWindow/CodeWindow';
import CommandLine from '../CommandLine/CommandLine';
import CtaLink, {CtaRow} from '../CtaLink/CtaLink';
import DevicePreview from '../DevicePreview/DevicePreview';
import NavyBand from '../NavyBand/NavyBand';
import {CREATE_COMMAND} from '../content';

import styles from './Hero.module.css';

// DevicePreview draws this app's output; keep the two in step.
const sample = `import {View, Text, Pressable} from 'embedded-react';

export default function App() {
  return (
    <View style={{flex: 1, padding: 20, backgroundColor: '#1a1a2e'}}>
      <Text style={{color: '#fff', fontSize: 24}}>
        Hello from an ESP32.
      </Text>
      <Pressable onPress={() => console.log('tapped')}>
        <Text style={{color: '#e94560', marginTop: 12}}>Tap me</Text>
      </Pressable>
    </View>
  );
}`;

export default function Hero(): ReactNode {
  return (
    <NavyBand as="header" className={styles.hero}>
      <div className={styles.grid}>
        <div>
          <p className={styles.pill}>Open source · Apache-2.0 · Beta</p>
          <Heading as="h1" className={styles.title}>
            <Wordmark onDark />
          </Heading>
          <p className={styles.lead}>React Native for embedded MCUs.</p>
          <p className={styles.sub}>
            Write React, flash it, and the UI runs on the chip. No browser, no phone, no operating system.
          </p>
          <CtaRow className={styles.buttons}>
            <CtaLink to="/getting-started">Get started</CtaLink>
            <CtaLink to="/playground" variant="ghost">
              Try it in the browser
            </CtaLink>
          </CtaRow>
          <CommandLine text={CREATE_COMMAND} />
        </div>
        {/* The code, with the device it produces overlapping its corner. */}
        <div className={styles.visual}>
          <CodeWindow title="src/App.jsx" code={sample} />
          <DevicePreview className={styles.device} />
        </div>
      </div>
    </NavyBand>
  );
}
