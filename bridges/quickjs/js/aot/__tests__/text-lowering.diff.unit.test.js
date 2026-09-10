/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Differential test for text lowering: JavaScript is the oracle. For a matrix of expressions the AOT is
 * asked to render, this compiles the emitted snprintf with `-Werror` and RUNS it, then compares the bytes
 * against what JS itself computes for the same values.
 *
 * Hand-picked assertions kept missing whole corners of this domain — `+` typing, boolean rendering, the
 * constant fold vs. shadowing, `%` escaping — because each one only pins the case someone thought of.
 * The matrix pins the rule instead. The C compile is what catches a format/argument mismatch; the run is
 * what catches a format that is well-typed but says the wrong thing.
 */

import {describe, it, expect} from 'vitest';
import {writeFileSync, mkdtempSync, rmSync} from 'node:fs';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {spawnSync, execFileSync} from 'node:child_process';
import {compileSource} from '../compile.mjs';

/** First working C compiler, or null (the suite still passes on a toolchain-less machine). */
function findCC() {
  for (const cc of ['C:\\mingw32\\bin\\gcc.exe', 'gcc', 'cc', 'clang']) {
    try {
      if (spawnSync(cc, ['--version'], {stdio: 'ignore'}).status === 0)
        return cc;
    } catch {
      /* try the next */
    }
  }
  return null;
}
const CC = findCC();

// Value sets, so anything branch-dependent is exercised both ways (and `%` appears inside a string slot).
const SETS = [
  {n: 3, f: 1.5, s: 'hi', on: false, on2: true},
  {n: 0, f: -2.25, s: '', on: true, on2: false},
  {n: -7, f: 0.5, s: 'x%y', on: true, on2: true},
];

const EXPRS = [
  `n`,
  `f`,
  `s`,
  `on`,
  `on2`,
  `null`,
  `undefined`,
  `true`,
  `false`,
  `42`,
  `'lit'`,
  `-1`,
  `1.25`,
  `'a' + n`,
  `'a' + f`,
  `'a' + s`,
  `'a' + on`,
  `'a' + on2`,
  `n + 'a'`,
  `f + 'a'`,
  `s + 'a'`,
  `on + 'a'`,
  `on2 + 'a'`,
  `n + s`,
  `s + n`,
  `n + f`,
  `f + n`,
  `on + n`,
  `n + on`,
  `on + on2`,
  `s + s`,
  // JS types `+` left to right: these only become concatenation at the first string operand.
  `n + 1 + ' ms'`,
  `'ms ' + (n + 1)`,
  `n + 1 + 2 + 'x'`,
  `'x' + n + 1 + 2`,
  `on + n + 'x'`,
  `'x' + on + n`,
  `n * 2 + '%'`,
  `(n + f) + 'u'`,
  `'v' + (+on)`,
  `'v' + (-n)`,
  `'v' + (!on)`,
  `+on`,
  `-n`,
  `!on`,
  `'c' + (n > 1)`,
  `'c' + (n === 3)`,
  `'c' + (s === 'hi')`,
  `n > 1`,
  `'t' + (on ? 'y' : 'z')`,
  `'t' + (n > 1 ? 1 : 2)`,
  `on ? 'y' : 'z'`,
  `'t' + (on ? on2 : !on2)`,
  `'l' + (on && on2)`,
  `'l' + (on || on2)`,
  `'l' + (n > 1 && n < 9)`,
  `n + '%'`,
  `'100%'`,
  `'%d' + n`,
  `s + '%'`,
  `s + null`,
  `'a' + undefined`,
  `s + null + n`,
  `'a' + n + '/' + f + '/' + s`,
  `'[' + on + ']' + n`,
];

const decls = v => `
  const [n, setN] = useState(${JSON.stringify(v.n)});
  const [f, setF] = useState(${v.f % 1 === 0 ? v.f + '.0' : v.f});
  const [s, setS] = useState(${JSON.stringify(v.s)});
  const [on, setOn] = useState(${v.on});
  const [on2, setOn2] = useState(${v.on2});`;

/** The expression as a <Text> child. */
const childApp = (expr, v) => `import {useState} from 'react';
import {Text} from 'embedded-react';
export function App() {${decls(v)}
  return (<Text>{${expr}}</Text>);
}`;

/** The same expression written into a string state slot from a handler (the scalarAssign path). */
const setterApp = (expr, v) => `import {useState} from 'react';
import {Text, Pressable} from 'embedded-react';
export function App() {${decls(v)}
  const [out, setOut] = useState('');
  return (<Pressable onPress={() => setOut(${expr})}><Text>{out}</Text></Pressable>);
}`;

/** The same expression rendered inside a child component, reached through the prop boundary. */
const childCompApp = (expr, v) => `import {useState} from 'react';
import {View, Text} from 'embedded-react';
function Row({n, f, s, on, on2}) { return (<Text>{${expr}}</Text>); }
export function App() {${decls(v)}
  return (<View><Row n={n} f={f} s={s} on={on} on2={on2} /></View>);
}`;

/** React's rule for a standalone child: null, undefined and booleans render as nothing. */
const jsChild = val =>
  val === null || val === undefined || typeof val === 'boolean'
    ? ''
    : String(val);

const MODES = {
  child: {
    app: childApp,
    re: /snprintf\(p\.text, sizeof\(p\.text\), ([\s\S]*?)\);\n/,
  },
  setter: {
    app: setterApp,
    re: /snprintf\(s_state\.out, sizeof\(s_state\.out\), ([\s\S]*?)\);\n/,
  },
  'child component': {
    app: childCompApp,
    re: /snprintf\(p\.text, sizeof\(p\.text\), ([\s\S]*?)\);\n/,
  },
};

describe('AOT self-referential string setter', () => {
  (CC ? it : it.skip)(
    `appends to its own slot the way JS does (${CC || 'no cc found'})`,
    () => {
      const c = compileSource(
        `import {useState} from 'react';
         import {Text, Pressable} from 'embedded-react';
         export function App() {
           const [out, setOut] = useState('ab');
           return (<Pressable onPress={() => setOut(out + '!')}><Text>{out}</Text></Pressable>);
         }`,
        'selfref',
      ).c;
      const block = c.match(/\{\s*\n\s*char next\[[\s\S]*?\n\s*\}/);
      expect(block, 'no temporary-buffer block emitted').toBeTruthy();
      const body = block[0].replace(/s_state\./g, 'S.');

      const ITER = 5;
      let prog = '#include <stdio.h>\n#include <string.h>\n';
      prog += 'struct St { char out[64]; };\n';
      prog +=
        'int main(void){ struct St S; snprintf(S.out, sizeof S.out, "%s", "ab");\n';
      prog += `  for (int i = 0; i < ${ITER}; i++) {\n${body}\n    printf("%s\\n", S.out); }\n  return 0; }\n`;

      const dir = mkdtempSync(join(tmpdir(), 'er-aot-selfref-'));
      try {
        const src = join(dir, 'sr.c');
        const bin = join(dir, 'sr');
        writeFileSync(src, prog);
        const build = spawnSync(
          CC,
          ['-Wall', '-Wextra', '-Wformat', '-Werror', '-o', bin, src],
          {encoding: 'utf8'},
        );
        expect(build.stderr || '').toBe('');
        expect(build.status).toBe(0);
        const got = execFileSync(bin, {encoding: 'utf8'}).trim().split('\n');

        let js = 'ab';
        const want = [];
        for (let i = 0; i < ITER; i++) {
          js = js + '!';
          want.push(js);
        }
        expect(got).toEqual(want);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );
});

describe('AOT text lowering matches JavaScript', () => {
  for (const [mode, {app, re}] of Object.entries(MODES)) {
    (CC ? it : it.skip)(
      `${mode}: every expression renders what JS renders (${CC || 'no cc found'})`,
      () => {
        const cases = [];
        for (let si = 0; si < SETS.length; si++) {
          const v = SETS[si];
          for (const expr of EXPRS) {
            const c = compileSource(app(expr, v), 'diff').c;
            const m = c.match(re);
            expect(
              m,
              `${mode} ${expr} @set${si}: no snprintf emitted`,
            ).toBeTruthy();
            const js = jsChild(
              Function(
                'n',
                'f',
                's',
                'on',
                'on2',
                `return (${expr});`,
              )(v.n, v.f, v.s, v.on, v.on2),
            );
            cases.push({
              expr,
              si,
              v,
              call: m[1].replace(/s_state\./g, 'S.'),
              js,
            });
          }
        }

        // One translation unit for the whole matrix: -Werror turns any format/argument mismatch into a
        // build failure, and running it compares the actual bytes.
        let prog = '#include <stdio.h>\n#include <string.h>\n';
        prog += 'struct St { int n; float f; char s[64]; int on; int on2; };\n';
        cases.forEach((c, i) => {
          const v = c.v;
          prog +=
            `static void case_${i}(void){ struct St S = {${v.n}, ${v.f}f, ` +
            `${JSON.stringify(v.s)}, ${v.on ? 1 : 0}, ${v.on2 ? 1 : 0}}; (void)S;\n` +
            `  char b[256]; snprintf(b, sizeof b, ${c.call}); printf("%d\\t%s\\n", ${i}, b); }\n`;
        });
        prog +=
          'int main(void){\n' +
          cases.map((_, i) => `  case_${i}();`).join('\n') +
          '\n  return 0; }\n';

        const dir = mkdtempSync(join(tmpdir(), 'er-aot-diff-'));
        try {
          const src = join(dir, 'matrix.c');
          const bin = join(dir, 'matrix');
          writeFileSync(src, prog);
          const build = spawnSync(
            CC,
            ['-Wall', '-Wextra', '-Wformat', '-Werror', '-o', bin, src],
            {encoding: 'utf8'},
          );
          expect(build.stderr || '').toBe('');
          expect(build.status).toBe(0);

          const got = new Map();
          for (const line of execFileSync(bin, {encoding: 'utf8'}).split(
            '\n',
          )) {
            if (!line) continue;
            const t = line.indexOf('\t');
            got.set(Number(line.slice(0, t)), line.slice(t + 1));
          }
          const bad = cases
            .map((c, i) => ({...c, got: got.get(i)}))
            .filter(c => c.got !== c.js)
            .map(
              c =>
                `${c.expr} @set${c.si}: C=${JSON.stringify(c.got)} JS=${JSON.stringify(c.js)}`,
            );
          expect(bad).toEqual([]);
        } finally {
          rmSync(dir, {recursive: true, force: true});
        }
      },
    );
  }
});
