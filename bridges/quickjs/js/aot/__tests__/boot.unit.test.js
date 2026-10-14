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

import {describe, it, expect} from 'vitest';
import {mkdtempSync, rmSync, writeFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
import {compileSource} from '../compile.mts';

// Values derived only from `screen` are worked out by er_app_build() at boot; what a site has to fold at
// compile time is folded at the build's panel and checked against the panel at boot.
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../../../..');
const IMPORTS = `import { useState } from 'react';
import { View, Text } from 'embedded-react';
`;
const PORTRAIT = {screen: {width: 240, height: 320}};

/**
 * Compiles an app for a 240×320 panel.
 *
 * @param src  The app.
 *
 * @returns The generated C.
 */
const compile = src => compileSource(src, 'boot', PORTRAIT).c;

/**
 * The body of the generated app_boot(), or null when the app has none.
 *
 * @param c  The generated C.
 *
 * @returns The body.
 */
const bootOf = c =>
  c.match(
    /static bool app_boot\(int screen_w, int screen_h\)\n\{\n([\s\S]*?)\n\}/,
  )?.[1] ?? null;

/** First working C compiler, or null. */
function findCC() {
  for (const cc of ['gcc', 'cc', 'clang']) {
    try {
      if (spawnSync(cc, ['--version'], {stdio: 'ignore'}).status === 0) {
        return cc;
      }
    } catch {
      /* not on PATH — try the next */
    }
  }
  return null;
}
const CC = findCC();

describe('AOT values worked out at boot', () => {
  it('works a screen-derived style value out at boot and sets it once', () => {
    const c = compile(`${IMPORTS}const CARD_W = screen.width - 40;
export function App() {
  return (<View style={{width: CARD_W}} />);
}`);
    expect(bootOf(c)).toContain('s_boot.screen_width = screen_w;');
    expect(bootOf(c)).toContain(
      's_boot.CARD_W = app_sub(s_boot.screen_width, 40);',
    );
    expect(bootOf(c)).toContain('return true;');
    expect(c).toContain('p.width = app_round_dim(s_boot.CARD_W);');
    expect(c).not.toContain('static void app_update');
  });

  it('works fractions out in double precision, as JS does', () => {
    const c = compile(`${IMPORTS}const S = screen.width / 320;
export function App() {
  return (<View style={{width: 100 * S}} />);
}`);
    expect(c).toContain('    double S;');
    expect(bootOf(c)).toContain(
      's_boot.S = ((double)(s_boot.screen_width) / (double)(320));',
    );
  });

  it('re-applies a prop that mixes a boot value with state on each update', () => {
    const c = compile(`${IMPORTS}const HALF = screen.width / 2;
export function App() {
  const [n, setN] = useState(1);
  return (<View style={{width: HALF * n}} />);
}`);
    expect(c).toMatch(
      /static void app_update[\s\S]*p\.width = app_round_dim\(\(s_boot\.HALF \* s_state\.n\)\);/,
    );
  });

  it('prints a boot value in text at build', () => {
    const c = compile(`${IMPORTS}export function App() {
  return (<Text>{screen.width}x{screen.height}</Text>);
}`);
    expect(c).toContain(
      'snprintf(p.text, sizeof(p.text), "%dx%d", s_boot.screen_width, s_boot.screen_height);',
    );
    expect(c).not.toContain('static void app_update');
  });

  it('picks each entry of a same-shape object at boot', () => {
    const c =
      compile(`${IMPORTS}const SZ = screen.width > 400 ? {title: 24, body: 16} : {title: 20, body: 16};
export function App() {
  return (<View><Text style={{fontSize: SZ.title}}>a</Text><Text style={{fontSize: SZ.body}}>b</Text></View>);
}`);
    expect(bootOf(c)).toContain(
      's_boot.SZ_title = ((s_boot.screen_width > 400) ? 24 : 20);',
    );
    expect(c).toContain('p.font_size = app_round_dim(s_boot.SZ_title);');
    expect(c).toContain('p.font_size = 16;');
    expect(bootOf(c)).toContain('return true;');
  });

  it('turns a top-level `if` that changes the tree into a guard on its test', () => {
    const c = compile(`${IMPORTS}const WIDE = screen.width > screen.height;
export function App() {
  if (WIDE) {
    return (<View><Text>wide</Text></View>);
  }
  return (<Text>tall</Text>);
}`);
    expect(c).toContain('"tall"');
    expect(c).not.toContain('"wide"');
    expect(bootOf(c)).toContain('return !s_boot.WIDE;');
    expect(c).toMatch(
      /if \(!app_boot\(screen_w, screen_h\)\)\n {4}\{\n {8}return false;/,
    );
  });

  it('guards a `!flag` branch on the flag itself', () => {
    const c = compile(`${IMPORTS}const TINY = screen.height < 260;
export function App() {
  return (<View>{!TINY && <Text>tagline</Text>}</View>);
}`);
    expect(bootOf(c)).toContain('return !s_boot.TINY;');
  });

  it('folds a value that must be constant at the build panel, and checks it at boot', () => {
    const c = compile(`${IMPORTS}export function App() {
  const [w, setW] = useState(screen.width);
  return (<Text>{w}</Text>);
}`);
    expect(c).toContain('.w = 240');
    expect(bootOf(c)).toContain('return (s_boot.screen_width == 240);');
  });

  it('assumes the branch that picked a value boot cannot work out', () => {
    const c =
      compile(`${IMPORTS}const ITEMS = screen.width > 400 ? ['a', 'b', 'c'] : ['a', 'b'];
export function App() {
  return (<View>{ITEMS.map(item => <Text key={item}>{item}</Text>)}</View>);
}`);
    expect(bootOf(c)).toContain('s_boot.value = (s_boot.screen_width > 400);');
    expect(bootOf(c)).toContain('return !s_boot.value;');
  });

  it('assumes the whole panel for a fold boot cannot check', () => {
    const c = compile(`${IMPORTS}export function App() {
  const [w, setW] = useState('w' + screen.width);
  return (<Text>{w}</Text>);
}`);
    expect(bootOf(c)).toContain('return (screen_w == 240 && screen_h == 320);');
  });

  it('gives an app that never reads screen no app_boot, and fits every panel', () => {
    const c = compile(`${IMPORTS}export function App() {
  return (<Text>hi</Text>);
}`);
    expect(bootOf(c)).toBeNull();
    expect(c).not.toContain('s_boot');
    expect(c).toContain(
      'bool er_app_fits(int screen_w, int screen_h)\n{\n    (void)screen_w;\n' +
        '    (void)screen_h;\n    return true;\n}',
    );
    expect(c).toContain('bool er_app_build(int screen_w, int screen_h)');
  });

  (CC ? it : it.skip)(
    `runs app_boot and er_app_fits at several panels (${CC || 'no cc found'})`,
    () => {
      const {c, h} = compileSource(
        `${IMPORTS}const WIDE = screen.width > screen.height;
const PAD = WIDE ? 24 : 12;
const CARD_W = screen.width - 2 * PAD;
export function App() {
  if (WIDE) {
    return (<View><Text>wide</Text></View>);
  }
  return (<Text style={{width: CARD_W}}>tall</Text>);
}`,
        'boot',
        PORTRAIT,
      );

      // Everything before er_app_build(), with the engine calls app_update() and the clock make stubbed out.
      const main = `
void er_props_default(ERProps* props) { (void)props; }
void er_node_set_props(ERNode* node, const ERProps* props) { (void)node; (void)props; }
uint64_t er_now_ms64(void) { return 0; }

int main(void)
{
    const int sizes[][2] = {{240, 320}, {480, 800}, {320, 240}};
    for (int i = 0; i < 3; i++)
    {
        printf("%dx%d=%d ", sizes[i][0], sizes[i][1], er_app_fits(sizes[i][0], sizes[i][1]));
    }
    app_boot(480, 800);
    printf("card=%d ", s_boot.CARD_W);
    app_boot(240, 320);
    er_app_fits(800, 480);
    printf("kept=%d\\n", s_boot.CARD_W);
    return 0;
}
`;
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-boot-'));
      try {
        writeFileSync(join(dir, 'app.gen.h'), h);
        writeFileSync(
          join(dir, 'app.c'),
          c.slice(0, c.indexOf('bool er_app_build(')) + main,
        );
        const exe = join(dir, 'app');
        const build = spawnSync(
          CC,
          [
            '-std=c99',
            '-I',
            join(root, 'engine', 'include'),
            '-I',
            join(root, 'engine', 'core'),
            '-o',
            exe,
            join(dir, 'app.c'),
            '-lm',
          ],
          {encoding: 'utf8'},
        );
        expect(build.stderr).toBe('');
        expect(spawnSync(exe, [], {encoding: 'utf8'}).stdout).toBe(
          '240x320=1 480x800=1 320x240=0 card=456 kept=216\n',
        );
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );
});
