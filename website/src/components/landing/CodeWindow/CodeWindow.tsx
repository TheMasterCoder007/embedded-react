import type {ReactNode} from 'react';
import {Highlight, themes} from 'prism-react-renderer';

import styles from './CodeWindow.module.css';

type Props = {
  title: string;
  code: string;
  language?: string;
};

/**
 * An editor-style code window that is dark in both color modes, which the theme's CodeBlock
 * cannot be: it follows the page.
 */
export default function CodeWindow({title, code, language = 'jsx'}: Props): ReactNode {
  return (
    <div className={styles.window}>
      <div className={styles.bar}>
        <span />
        <span />
        <span />
        <em>{title}</em>
      </div>
      <Highlight theme={themes.nightOwl} code={code} language={language}>
        {({tokens, getLineProps, getTokenProps}) => (
          <pre className={styles.code}>
            {tokens.map((line, i) => (
              <div key={i} {...getLineProps({line})}>
                {line.map((token, k) => (
                  <span key={k} {...getTokenProps({token})} />
                ))}
              </div>
            ))}
          </pre>
        )}
      </Highlight>
    </div>
  );
}
