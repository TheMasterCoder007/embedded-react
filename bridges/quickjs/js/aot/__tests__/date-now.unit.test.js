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
import {compileSource} from '../compile.mjs';

// Date.now() / performance.now() in Flow B: the engine clock as 64-bit whole milliseconds, the same surface
// Flow A's lite profile gives. These assert on the generated C; cc-compile.smoke compiles it, and
// text-lowering.diff runs the math against JavaScript.
const PRE = `import { useState, useEffect, useRef } from 'react';
import { View, Text, Pressable } from 'embedded-react';
`;
const gen = src => compileSource(src, 'test').c;
const app = (body, jsx) => `${PRE}
export function App() {
  ${body}
  return (${jsx});
}`;

describe('AOT Date.now() / performance.now()', () => {
  it('reads the engine clock, and widens the state a setter stores it in', () => {
    const c = gen(
      app(
        'const [t, setT] = useState(0);',
        '<Pressable onPress={() => setT(Date.now())}><Text>{t}</Text></Pressable>',
      ),
    );
    expect(c).toContain('    int64_t t;');
    expect(c).toContain('s_state.t = app_date_now();');
    expect(c).toContain('"%lld", (long long)(s_state.t)');
    expect(c).toContain('static int64_t app_date_now(void)');
    expect(c).not.toContain('app_perf_now'); // only the readers the app calls
  });

  it('widens a slot that is read before the setter that stores the timestamp', () => {
    const c = gen(
      app(
        'const [t, setT] = useState(0);',
        '<View><Text>{t}</Text><Pressable onPress={() => setT(Date.now())}><Text>set</Text></Pressable></View>',
      ),
    );
    expect(c).toContain('    int64_t t;');
    expect(c).not.toMatch(/"%d", s_state\.t\b/);
  });

  it('keeps performance.now() in a ref and a timer callback', () => {
    const c = gen(
      app(
        `const [ms, setMs] = useState(0);
  const start = useRef(0);
  useEffect(() => {
    start.current = performance.now();
    const id = setInterval(() => setMs(performance.now() - start.current), 1000);
    return () => clearInterval(id);
  }, []);`,
        '<View><Text>{ms}</Text></View>',
      ),
    );
    expect(c).toContain('static int64_t s_ref_start = 0;');
    expect(c).toContain('s_ref_start = app_perf_now();');
    expect(c).toContain('s_state.ms = (app_perf_now() - s_ref_start);');
    expect(c).toContain('    int64_t ms;');
  });

  it('snapshots a 64-bit dependency of a dep-driven effect in a 64-bit slot', () => {
    const c = gen(
      app(
        `const [t, setT] = useState(0);
  const [n, setN] = useState(0);
  useEffect(() => { setN(v => v + 1); }, [t]);`,
        '<Pressable onPress={() => setT(Date.now())}><Text>{n}</Text></Pressable>',
      ),
    );
    expect(c).toContain('static int64_t s_eff0_d0;');
    expect(c).toContain('int64_t er_d0 = s_state.t;');
  });

  it('declares a handler local 64-bit, and `% constant` narrows back to int', () => {
    const c = gen(
      app(
        'const [sec, setSec] = useState(0);',
        '<Pressable onPress={() => { const now = Date.now(); setSec(Math.floor(now / 1000) % 60); }}><Text>{sec}</Text></Pressable>',
      ),
    );
    expect(c).toContain('int64_t l_now = app_date_now();');
    expect(c).toContain(
      's_state.sec = ((int)(app_floordiv64(l_now, 1000) % 60));',
    );
    expect(c).toContain('    int sec;');
    expect(c).toContain('static int64_t app_floordiv64(int64_t a, int64_t b)');
  });

  it('re-reads the clock in text on every update, as a Flow A re-render does', () => {
    const c = gen(
      app(
        'const [n, setN] = useState(0);',
        '<Pressable onPress={() => setN(n + 1)}><Text>{Math.floor(Date.now() / 60000) % 60}</Text></Pressable>',
      ),
    );
    const update = c.match(/static void app_update\(void\)\n\{[\s\S]*?\n\}/);
    expect(update, 'no app_update emitted').toBeTruthy();
    expect(update[0]).toContain(
      '"%d", ((int)(app_floordiv64(app_date_now(), 60000) % 60))',
    );
  });

  it('keeps min / max / abs exact over a timestamp', () => {
    const c = gen(
      app(
        `const [left, setLeft] = useState(0);
  const deadline = useRef(0);`,
        '<Pressable onPress={() => setLeft(Math.max(0, Math.abs(deadline.current - Date.now())))}><Text>{left}</Text></Pressable>',
      ),
    );
    expect(c).toContain(
      's_state.left = app_max64(0, app_abs64((s_ref_deadline - app_date_now())));',
    );
  });

  it('compares a timestamp in a handler condition', () => {
    const c = gen(
      app(
        `const [n, setN] = useState(0);
  const last = useRef(0);`,
        '<Pressable onPress={() => { if (Date.now() - last.current < 300) setN(n + 1); last.current = Date.now(); }}><Text>{n}</Text></Pressable>',
      ),
    );
    expect(c).toContain('if (((app_date_now() - s_ref_last) < 300))');
    expect(c).toContain('static int64_t s_ref_last = 0;');
  });

  it('passes a timestamp into a child component, and widens the child’s own state', () => {
    const c = gen(`${PRE}
function Stamp({at}) {
  const [t, setT] = useState(0);
  return (
    <Pressable onPress={() => setT(Date.now())}>
      <Text>{at}</Text>
      <Text>{t}</Text>
    </Pressable>
  );
}
export function App() {
  const [t0, setT0] = useState(0);
  return (
    <View>
      <Pressable onPress={() => setT0(Date.now())}><Text>go</Text></Pressable>
      <Stamp at={t0} />
    </View>
  );
}`);
    expect(c).toContain('    int64_t t0;');
    expect(c).toContain('"%lld", (long long)(s_state.t0)');
    const child = c.match(/ {4}int64_t (\w+);/g).filter(f => !f.includes('t0'));
    expect(child.length, 'the child state was not widened').toBe(1);
  });

  it('always exports the wall-clock setter, and the header declares it', () => {
    const r = compileSource(app('', '<View><Text>x</Text></View>'), 'test');
    expect(r.c).toContain('static int64_t s_wall_offset_ms;');
    expect(r.c).toContain('void er_app_set_wall_clock(int64_t epoch_ms)');
    expect(r.c).toContain(
      's_wall_offset_ms = epoch_ms - (int64_t)er_now_ms64();',
    );
    expect(r.c).not.toContain('app_date_now');
    expect(r.h).toContain('#include <stdint.h>');
    expect(r.h).toContain('void er_app_set_wall_clock(int64_t epoch_ms);');
  });

  it('allows a timestamp narrowed by `% constant` in a style', () => {
    const c = gen(
      app('', '<View style={{width: Date.now() % 100, height: 4}} />'),
    );
    expect(c).toContain('((int)(app_date_now() % 100))');
  });

  it.each([
    ['a plain `/`', '<Text>{Date.now() / 1000}</Text>', /`\/` on a 64-bit/],
    ['a float', '<Text>{Date.now() * 0.5}</Text>', /mixed with a float/],
    [
      'a float comparison',
      "<Text>{Date.now() > 0.5 ? 'a' : 'b'}</Text>",
      /mixed with a float/,
    ],
    [
      'Math.round of a division',
      '<Text>{Math.round(Date.now() / 1000)}</Text>',
      /Math\.round\(a \/ b\)/,
    ],
    [
      'Math.sqrt',
      '<Text>{Math.sqrt(Date.now())}</Text>',
      /Math\.sqrt\(\.\.\.\) on a 64-bit/,
    ],
    ['`% 0`', '<Text>{Date.now() % 0}</Text>', /`% 0`/],
    ['new Date()', '<Text>{new Date()}</Text>', /Date objects/],
    ['Date()', '<Text>{Date()}</Text>', /Date objects/],
    ['Date.parse', "<Text>{Date.parse('2026')}</Text>", /Date objects/],
    [
      'a timestamp as a style value',
      '<View style={{width: Date.now()}} />',
      /cannot be used here/,
    ],
  ])('refuses %s', (_, jsx, err) => {
    expect(() => gen(app('', `<View>${jsx}</View>`))).toThrow(err);
  });

  it('refuses to store a timestamp in a float or boolean state', () => {
    expect(() =>
      gen(
        app(
          'const [f, setF] = useState(0.0);',
          '<Pressable onPress={() => setF(Date.now())}><Text>{f}</Text></Pressable>',
        ),
      ),
    ).toThrow(/cannot be stored in state "f"/);
    expect(() =>
      gen(
        app(
          'const [on, setOn] = useState(false);',
          '<Pressable onPress={() => setOn(Date.now())}><Text>x</Text></Pressable>',
        ),
      ),
    ).toThrow(/cannot be stored in state "on"/);
  });

  it('refuses `/=` on a 64-bit ref', () => {
    expect(() =>
      gen(
        app(
          'const r = useRef(0);',
          '<Pressable onPress={() => { r.current = Date.now(); r.current /= 2; }}><Text>x</Text></Pressable>',
        ),
      ),
    ).toThrow(/`\/=` on a 64-bit/);
  });
});
