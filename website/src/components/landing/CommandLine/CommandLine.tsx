import {useState, type ReactNode} from 'react';

import styles from './CommandLine.module.css';

/** A shell command with a copy button, styled for a navy band. */
export default function CommandLine({text}: {text: string}): ReactNode {
  const [copied, setCopied] = useState(false);
  const copy = () => {
    navigator.clipboard?.writeText(text).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    });
  };
  return (
    <div className={styles.command}>
      <span className={styles.prompt} aria-hidden="true">
        $
      </span>
      <code>{text}</code>
      <button type="button" onClick={copy} aria-label="Copy command">
        {copied ? 'Copied' : 'Copy'}
      </button>
    </div>
  );
}
