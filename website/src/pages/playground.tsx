import type {ReactNode} from 'react';
import BrowserOnly from '@docusaurus/BrowserOnly';
import Layout from '@theme/Layout';

import styles from './playground.module.css';

// The editor and the engine only exist in the browser; the server render is a placeholder.
export default function PlaygroundPage(): ReactNode {
  return (
    <Layout
      title="Playground"
      description="Edit a demo app and watch it run live on the C engine, compiled to WebAssembly."
      noFooter>
      <BrowserOnly fallback={<div className={styles.loading}>Loading the playground…</div>}>
        {() => {
          // eslint-disable-next-line @typescript-eslint/no-require-imports, global-require
          const Playground = require('@site/src/components/Playground/Playground').default;
          return <Playground />;
        }}
      </BrowserOnly>
    </Layout>
  );
}
