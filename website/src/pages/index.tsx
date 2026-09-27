import type {ReactNode} from 'react';
import Layout from '@theme/Layout';

import Closing from '@site/src/components/landing/Closing/Closing';
import Features from '@site/src/components/landing/Features/Features';
import Flows from '@site/src/components/landing/Flows/Flows';
import Hardware from '@site/src/components/landing/Hardware/Hardware';
import Hero from '@site/src/components/landing/Hero/Hero';

export default function Home(): ReactNode {
  return (
    <Layout description="React Native for embedded MCUs. Write React, flash it, and the UI runs on the chip.">
      <Hero />
      <main>
        <Features />
        <Flows />
        <Hardware />
        <Closing />
      </main>
    </Layout>
  );
}
