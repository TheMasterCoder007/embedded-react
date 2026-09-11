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

/**
 * The diagnostic suppressions the real app.gen.c carries, lifted out of a generated file rather than
 * copied. Fixtures below compile EXTRACTED snippets, so without this they face a stricter environment
 * than the code they came from: GCC's -Wformat-truncation fires on the intended fixed-slot truncation
 * and -Werror turns it into a failure, while clang has no such warning and stays silent. Deriving it
 * keeps the two from drifting if compile.mjs ever changes what it suppresses.
 */
const GENERATED_PRAGMAS = (() => {
  const c = compileSource(
    `import {Text} from 'embedded-react';
     export function App() { return (<Text>x</Text>); }`,
    'pragmas',
  ).c;
  const m = c.match(/#if defined\(__GNUC__\)[\s\S]*?#endif\n/);
  return m ? m[0] : '';
})();

/** The file-local `app_*` helpers a generated file defines, by name. */
const helperDefs = c =>
  new Map(
    [...c.matchAll(/static [\w ]+? (app_\w+)\([^)]*\)\n\{[\s\S]*?\n\}\n/g)].map(
      m => [m[1], m[0]],
    ),
  );

/** The helpers any of `calls` uses, as C source: an unused static function would fail -Werror. */
const usedHelpers = (defs, calls) =>
  [...defs]
    .filter(([name]) => calls.some(c => c.includes(`${name}(`)))
    .map(([, def]) => def)
    .join('');

const HELPER_INCLUDES =
  '#include <limits.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n';

/**
 * UBSan flags that build here, or none. With them a signed overflow that gets through aborts the run
 * instead of printing a plausible number. MinGW has no UBSan runtime, so the trap-only form is tried too.
 */
const UBSAN = (() => {
  if (!CC) return [];
  const dir = mkdtempSync(join(tmpdir(), 'er-aot-ubsan-probe-'));
  try {
    const src = join(dir, 'p.c');
    writeFileSync(src, 'int main(void){return 0;}\n');
    for (const flags of [
      ['-fsanitize=undefined', '-fno-sanitize-recover=all'],
      ['-fsanitize=undefined', '-fsanitize-undefined-trap-on-error'],
    ])
      if (spawnSync(CC, [...flags, '-o', join(dir, 'p'), src]).status === 0)
        return flags;
    return [];
  } finally {
    rmSync(dir, {recursive: true, force: true});
  }
})();

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
      let prog =
        '#include <stdio.h>\n#include <string.h>\n' + GENERATED_PRAGMAS;
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
        const defs = new Map();
        for (let si = 0; si < SETS.length; si++) {
          const v = SETS[si];
          for (const expr of EXPRS) {
            const c = compileSource(app(expr, v), 'diff').c;
            for (const [name, def] of helperDefs(c)) defs.set(name, def);
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
        let prog =
          HELPER_INCLUDES +
          GENERATED_PRAGMAS +
          usedHelpers(
            defs,
            cases.map(c => c.call),
          );
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
            [
              '-Wall',
              '-Wextra',
              '-Wformat',
              '-Werror',
              ...UBSAN,
              '-o',
              bin,
              src,
            ],
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

describe('AOT 64-bit time math matches JavaScript', () => {
  // Negative values too: Date.now() goes back when the host re-sets the wall clock, so `now - start` can be
  // below zero, and that is where C's `/` and Math.floor part ways.
  const VALUES = [
    0, 1, 999, 1000, 1500, -1, -1000, -1500, 1700000123456, -1700000123456,
  ];
  const EXPRS64 = [
    `t`,
    `t + 1`,
    `t - 1700000000000`,
    `t * 2`,
    `t % 1000`,
    `t % -7`,
    `Math.floor(t / 1000)`,
    `Math.floor(t / -7)`,
    `Math.floor(t / 60000) % 60`,
    `Math.floor(t)`,
    `Math.abs(t)`,
    `Math.max(0, t)`,
    `Math.min(t, 5)`,
    `'t=' + t + ' ms'`,
    `t > 1000 ? 'late' : 'early'`,
  ];

  (CC ? it : it.skip)(
    `every expression renders what JS renders (${CC || 'no cc found'})`,
    () => {
      const cases = [];
      const helpers = new Map();
      for (const expr of EXPRS64) {
        // The setter stores Date.now(), which widens `t` to int64_t; the text is what gets checked.
        const c = compileSource(
          `import {useState} from 'react';
import {Text, Pressable} from 'embedded-react';
export function App() {
  const [t, setT] = useState(0);
  return (<Pressable onPress={() => setT(Date.now())}><Text>{${expr}}</Text></Pressable>);
}`,
          'diff64',
        ).c;
        expect(c).toContain('    int64_t t;');
        const m = c.match(MODES.child.re);
        expect(m, `${expr}: no snprintf emitted`).toBeTruthy();
        for (const [name, def] of helperDefs(c)) helpers.set(name, def);
        for (const v of VALUES)
          cases.push({
            expr,
            v,
            call: m[1].replace(/s_state\./g, 'S.'),
            js: String(Function('t', `return (${expr});`)(v)),
          });
      }

      let prog =
        HELPER_INCLUDES +
        GENERATED_PRAGMAS +
        usedHelpers(
          helpers,
          cases.map(c => c.call),
        );
      prog += 'struct St { int64_t t; };\n';
      cases.forEach((c, i) => {
        prog +=
          `static void case_${i}(void){ struct St S = {${c.v}LL}; (void)S;\n` +
          `  char b[256]; snprintf(b, sizeof b, ${c.call}); printf("%d\\t%s\\n", ${i}, b); }\n`;
      });
      prog +=
        'int main(void){\n' +
        cases.map((_, i) => `  case_${i}();`).join('\n') +
        '\n  return 0; }\n';

      const dir = mkdtempSync(join(tmpdir(), 'er-aot-diff64-'));
      try {
        const src = join(dir, 'time.c');
        const bin = join(dir, 'time');
        writeFileSync(src, prog);
        const build = spawnSync(
          CC,
          ['-Wall', '-Wextra', '-Wformat', '-Werror', ...UBSAN, '-o', bin, src],
          {encoding: 'utf8'},
        );
        expect(build.stderr || '').toBe('');
        expect(build.status).toBe(0);

        const got = new Map();
        for (const line of execFileSync(bin, {encoding: 'utf8'}).split('\n')) {
          if (!line) continue;
          const t = line.indexOf('\t');
          got.set(Number(line.slice(0, t)), line.slice(t + 1));
        }
        const bad = cases
          .map((c, i) => ({...c, got: got.get(i)}))
          .filter(c => c.got !== c.js)
          .map(
            c =>
              `${c.expr} @t=${c.v}: C=${JSON.stringify(c.got)} JS=${JSON.stringify(c.js)}`,
          );
        expect(bad).toEqual([]);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );
});

describe('AOT timer delay from a timestamp matches Flow A', () => {
  // Flow A's setTimeout runs its delay through JS_ToInt32 and floors a negative at 0 (timer_register in
  // native_ui_bridge.c); app_delay_ms64 has to land on the same number.
  const VALUES = [
    0, 1, 1000, 2147483647, 2147483648, 4294967295, 4294967301, -1, -1000,
    1700000123456, -1700000123456,
  ];

  (CC ? it : it.skip)(
    `converts every delay the way Flow A does (${CC || 'no cc found'})`,
    () => {
      const c = compileSource(
        `import {useState} from 'react';
import {Text, Pressable} from 'embedded-react';
export function App() {
  const [t, setT] = useState(0);
  return (<Pressable onPress={() => { setT(Date.now()); setTimeout(() => setT(0), t - Date.now()); }}><Text>{t}</Text></Pressable>);
}`,
        'delay64',
      ).c;
      expect(c).toContain('er_timer_add((int)(app_delay_ms64(');
      const def = c.match(
        /static int app_delay_ms64\(int64_t v\)\n\{[\s\S]*?\n\}\n/,
      );
      expect(def, 'no app_delay_ms64 emitted').toBeTruthy();

      const prog =
        '#include <stdint.h>\n#include <stdio.h>\n' +
        def[0] +
        'int main(void){\n' +
        VALUES.map(v => `  printf("%d\\n", app_delay_ms64(${v}LL));`).join(
          '\n',
        ) +
        '\n  return 0; }\n';
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-delay64-'));
      try {
        const src = join(dir, 'delay.c');
        const bin = join(dir, 'delay');
        writeFileSync(src, prog);
        const build = spawnSync(
          CC,
          ['-Wall', '-Wextra', '-Werror', '-o', bin, src],
          {encoding: 'utf8'},
        );
        expect(build.stderr || '').toBe('');
        expect(build.status).toBe(0);
        const got = execFileSync(bin, {encoding: 'utf8'}).trim().split('\n');
        const want = VALUES.map(v => String(Math.max(0, v | 0)));
        expect(got).toEqual(want);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );
});

describe('AOT whole-number overflow saturates', () => {
  /** Builds `prog` under -Werror, with UBSan where it builds, and runs it: `index\ttext` lines → Map. */
  function buildAndRun(prog, tag) {
    const dir = mkdtempSync(join(tmpdir(), `er-aot-${tag}-`));
    try {
      const src = join(dir, `${tag}.c`);
      const bin = join(dir, tag);
      writeFileSync(src, prog);
      const build = spawnSync(
        CC,
        ['-Wall', '-Wextra', '-Wformat', '-Werror', ...UBSAN, '-o', bin, src],
        {encoding: 'utf8'},
      );
      expect(build.stderr || '').toBe('');
      expect(build.status).toBe(0);
      const got = new Map();
      for (const line of execFileSync(bin, {encoding: 'utf8'}).split('\n')) {
        if (!line) continue;
        const t = line.indexOf('\t');
        got.set(Number(line.slice(0, t)), line.slice(t + 1));
      }
      return got;
    } finally {
      rmSync(dir, {recursive: true, force: true});
    }
  }

  // One operator each, so the model is the exact result clamped to the type's range. JS would keep the
  // bigger number; what is pinned here is that C gives a defined one.
  const EXPRS = [
    ['n + 1', n => n + 1n],
    ['n - 1', n => n - 1n],
    ['1 - n', n => 1n - n],
    ['n + n', n => n + n],
    ['n * 100000', n => n * 100000n],
    ['n * n', n => n * n],
    ['n * -1', n => -n],
    ['-n', n => -n],
  ];
  // Each minimum is spelled the way C needs it: `-2147483648` is `-` applied to a number too big for an int.
  const WIDTHS = {
    int: {
      bits: 32n,
      field: 'int n;',
      values: [
        0n,
        7n,
        -7n,
        21474n,
        21475n,
        -21475n,
        46341n,
        2n ** 31n - 1n,
        -(2n ** 31n),
      ],
      exprs: EXPRS,
      // Nothing stores a timestamp in `n`, so it stays an int.
      body: expr => `<Text>{${expr}}</Text>`,
      lit: v => (v === -(2n ** 31n) ? '(-2147483647 - 1)' : String(v)),
    },
    int64: {
      bits: 64n,
      field: 'int64_t n;',
      values: [
        0n,
        7n,
        -7n,
        1789075980000n,
        3037000500n,
        -3037000500n,
        2n ** 62n,
        2n ** 63n - 1n,
        -(2n ** 63n),
      ],
      exprs: [...EXPRS, ['Math.abs(n)', n => (n < 0n ? -n : n)]],
      // Storing Date.now() widens `n` to int64_t.
      body: expr =>
        `<Pressable onPress={() => setN(Date.now())}><Text>{${expr}}</Text></Pressable>`,
      lit: v =>
        v === -(2n ** 63n) ? '(-9223372036854775807LL - 1)' : `${v}LL`,
    },
  };

  for (const [name, w] of Object.entries(WIDTHS))
    (CC ? it : it.skip)(
      `${name}: + - * and negation clamp to the range (${CC || 'no cc found'})`,
      () => {
        const lo = -(2n ** (w.bits - 1n));
        const hi = 2n ** (w.bits - 1n) - 1n;
        const defs = new Map();
        const cases = [];
        for (const [expr, model] of w.exprs) {
          const c = compileSource(
            `import {useState} from 'react';
import {Text, Pressable} from 'embedded-react';
export function App() {
  const [n, setN] = useState(0);
  return (${w.body(expr)});
}`,
            'overflow',
          ).c;
          expect(c).toContain(`    ${w.field}`);
          const m = c.match(MODES.child.re);
          expect(m, `${expr}: no snprintf emitted`).toBeTruthy();
          for (const [k, def] of helperDefs(c)) defs.set(k, def);
          for (const v of w.values) {
            const x = model(v);
            cases.push({
              expr,
              v,
              call: m[1].replace(/s_state\./g, 'S.'),
              want: String(x < lo ? lo : x > hi ? hi : x),
            });
          }
        }
        const helpers = usedHelpers(
          defs,
          cases.map(c => c.call),
        );
        // GCC and Clang take the overflow builtin; the division fallback other compilers get must agree.
        const variants = [helpers];
        if (helpers.includes('__builtin_mul_overflow'))
          variants.push(
            helpers.replace(/^#if defined\(__clang__\).*$/m, '#if 0'),
          );
        for (const h of variants) {
          let prog = HELPER_INCLUDES + GENERATED_PRAGMAS + h;
          prog += `struct St { ${w.field} };\n`;
          cases.forEach((c, i) => {
            prog +=
              `static void case_${i}(void){ struct St S = {${w.lit(c.v)}}; (void)S;\n` +
              `  char b[64]; snprintf(b, sizeof b, ${c.call}); printf("%d\\t%s\\n", ${i}, b); }\n`;
          });
          prog +=
            'int main(void){\n' +
            cases.map((_, i) => `  case_${i}();`).join('\n') +
            '\n  return 0; }\n';
          const got = buildAndRun(prog, `overflow-${name}`);
          const bad = cases
            .map((c, i) => ({...c, got: got.get(i)}))
            .filter(c => c.got !== c.want)
            .map(c => `${c.expr} @n=${c.v}: C=${c.got} want=${c.want}`);
          expect(bad).toEqual([]);
        }
      },
    );

  (CC ? it : it.skip)(
    `a handler storing an overflowing product keeps a clamped value (${CC || 'no cc found'})`,
    () => {
      const c = compileSource(
        `import {useState} from 'react';
import {Text, Pressable} from 'embedded-react';
export function App() {
  const [t, setT] = useState(0);
  const [n, setN] = useState(0);
  return (<Pressable onPress={() => { setT(Date.now() * 10000000); setN(n * 100000); }}><Text>{n}</Text></Pressable>);
}`,
        'handler-overflow',
      ).c;
      const body = c.match(
        / {4}s_state\.t = [^\n]+\n {4}s_state\.n = [^\n]+\n/,
      );
      expect(body, 'no setter statements emitted').toBeTruthy();
      const defs = helperDefs(c);
      defs.delete('app_date_now'); // pinned below instead of reading the engine clock
      const prog =
        HELPER_INCLUDES +
        usedHelpers(defs, [body[0]]) +
        'static int64_t app_date_now(void) { return 1789075980000LL; }\n' +
        'struct St { int64_t t; int n; };\n' +
        'int main(void){ struct St S = {0, 30000};\n' +
        body[0].replace(/s_state\./g, 'S.') +
        '    printf("0\\tt=%lld n=%d\\n", (long long)S.t, S.n);\n    return 0; }\n';
      expect(buildAndRun(prog, 'handler-overflow').get(0)).toBe(
        't=9223372036854775807 n=2147483647',
      );
    },
  );
});
