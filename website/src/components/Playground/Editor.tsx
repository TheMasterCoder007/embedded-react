import {useEffect, useRef, type ReactNode} from 'react';
import {EditorView, basicSetup} from 'codemirror';
import {EditorState, Compartment} from '@codemirror/state';
import {keymap} from '@codemirror/view';
import {indentWithTab} from '@codemirror/commands';
import {javascript} from '@codemirror/lang-javascript';
import {oneDark} from '@codemirror/theme-one-dark';

import styles from './Editor.module.css';

type Props = {
  /** Which file is open; changing it swaps the document. */
  path: string;
  value: string;
  dark: boolean;
  onChange: (path: string, value: string) => void;
};

// Editor chrome matches the site: the wordmark's mono font, and the navy palette in dark mode.
const chrome = EditorView.theme({
  '&': {height: '100%', fontSize: '13px'},
  // No ligatures: a beginner should see `>=` as written, not `≥`.
  '.cm-scroller': {fontFamily: 'var(--er-font-mono)', fontVariantLigatures: 'none', lineHeight: '1.55'},
  '.cm-content': {padding: '12px 0'},
  '.cm-gutters': {border: 'none'},
});
const navy = EditorView.theme(
  {
    '&': {backgroundColor: '#07111f'},
    '.cm-gutters': {backgroundColor: '#07111f', color: '#3d5a75'},
    '.cm-activeLineGutter': {backgroundColor: '#0d2035'},
    '.cm-activeLine': {backgroundColor: 'rgba(56, 189, 248, 0.05)'},
  },
  {dark: true},
);

/** A CodeMirror editor for one file at a time. Uncontrolled: it owns the document between changes. */
export default function Editor({path, value, dark, onChange}: Props): ReactNode {
  const host = useRef<HTMLDivElement>(null);
  const view = useRef<EditorView | null>(null);
  const theme = useRef(new Compartment());
  const latest = useRef({path, onChange});
  latest.current = {path, onChange};

  const makeState = (doc: string) =>
    EditorState.create({
      doc,
      extensions: [
        basicSetup,
        keymap.of([indentWithTab]),
        javascript({jsx: true, typescript: /\.tsx?$/.test(latest.current.path)}),
        chrome,
        theme.current.of(dark ? [oneDark, navy] : []),
        EditorView.updateListener.of((u) => {
          if (u.docChanged) latest.current.onChange(latest.current.path, u.state.doc.toString());
        }),
      ],
    });

  useEffect(() => {
    if (!host.current) return undefined;
    const v = new EditorView({
      parent: host.current,
      state: makeState(value),
    });
    view.current = v;
    return () => v.destroy();
    // Created once; file switches and theme changes are dispatched below.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Switching files replaces the whole document (and its undo history).
  const shownPath = useRef(path);
  useEffect(() => {
    const v = view.current;
    if (!v || shownPath.current === path) return;
    shownPath.current = path;
    v.setState(makeState(value));
  }, [path, value, dark]);

  // A reset from outside (same file, new text) replaces the text in place.
  useEffect(() => {
    const v = view.current;
    if (!v || shownPath.current !== path) return;
    const current = v.state.doc.toString();
    if (current !== value) v.dispatch({changes: {from: 0, to: current.length, insert: value}});
  }, [path, value]);

  useEffect(() => {
    view.current?.dispatch({effects: theme.current.reconfigure(dark ? [oneDark, navy] : [])});
  }, [dark]);

  return <div ref={host} className={styles.editor} />;
}
