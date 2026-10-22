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
 * AOT codegen text is unit-tested by regex, but a regex can't catch a generated call that no longer matches
 * an engine signature (e.g., a stale er_node_set_vector_ops arity). This smoke test closes that gap: it
 * actually runs a C compiler over the generated app.gen.c. It targets the thermostat's solo (240×320)
 * branch because that exercises a broad slice of the emission. The compiler step skips when no C compiler
 * is present (so the suite still passes in a toolchain-less environment), but the compile-to-C always runs.
 */

import {describe, it, expect} from 'vitest';
import {readFileSync, writeFileSync, mkdtempSync, rmSync} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
import {compileSource, bakeSvgArtifacts} from '../compile.mts';
import {printWaitDeclarations} from '../compiler/c/animations.mts';
import {APP_LIMIT_DEFINITION} from '../compiler/c/statements.mts';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../../../..');
const demosDir = join(root, 'demos');
const engineInc = join(root, 'engine', 'include');
const engineCore = join(root, 'engine', 'core');

/** Bake the thermostat's <Svg source> imports and AOT-compile its compact (240×320) branch to C. */
async function emitThermostat() {
  const src = readFileSync(join(demosDir, 'thermostat', 'App.jsx'), 'utf8');
  const svgArtifacts = await bakeSvgArtifacts(
    src,
    join(demosDir, 'thermostat'),
  );
  return compileSource(src, 'thermostat', {
    svgArtifacts,
    screen: {width: 240, height: 320},
    filename: 'demos/thermostat/App.jsx',
  });
}

/** First, working C compiler from a small candidate list, or null. Tries the repo's MinGW first. */
function findCC() {
  for (const cc of ['C:\\mingw32\\bin\\gcc.exe', 'gcc', 'cc', 'clang']) {
    try {
      if (spawnSync(cc, ['--version'], {stdio: 'ignore'}).status === 0)
        return cc;
    } catch {
      /* not on PATH — try the next */
    }
  }
  return null;
}
const CC = findCC();

/**
 * Warnings that only real GCC implements, added when the compiler accepts them. `gcc` is clang on macOS,
 * which silently has no -Wformat-truncation — and ESP-IDF builds with GCC and -Werror, so a format this
 * suite called clean could still fail on the device. Probing keeps the flag off clang (where an unknown
 * warning group would itself be an error under -Werror) without hard-coding a toolchain.
 */
const GCC_FORMAT_FLAGS = (() => {
  const probe = join(tmpdir(), `er-flagprobe-${process.pid}.c`);
  try {
    writeFileSync(probe, 'int main(void){return 0;}\n');
    const r = spawnSync(
      CC ?? 'cc',
      [
        '-Wformat-truncation=2',
        '-Wformat-overflow=2',
        '-Werror',
        '-fsyntax-only',
        probe,
      ],
      {encoding: 'utf8'},
    );
    return r.status === 0
      ? ['-Wformat-truncation=2', '-Wformat-overflow=2']
      : [];
  } catch {
    return [];
  } finally {
    rmSync(probe, {force: true});
  }
})();

describe('AOT generated C compiles', () => {
  it('emits the thermostat solo dial as a native, state-driven arc node', async () => {
    const r = await emitThermostat();
    expect(r.c).toContain('app_node_create(ER_NODE_ARC)');
    expect(r.c).toContain('p.arc_range ='); // AUTO's two-setpoint band
    expect(r.c).toContain('p.arc_value ='); // the setpoint drives it directly
    expect(r.c).toContain('ER_EVENT_VALUE_CHANGE');
    expect(r.c).not.toMatch(/static void build_svg\d+\(void\)/); // no hand-built op-tape any more
  });

  // The dial moved off <Svg>, so keep an explicit vector fixture: this is what catches a generated call
  // that no longer matches an engine signature (e.g. a stale er_node_set_vector_ops arity).
  it('still emits a valid vector op-tape for an <Svg> app', () => {
    const r = compileSource(
      `import { View, Svg, Path, Circle } from 'embedded-react';
       export function App() {
         return (
           <View style={{ flex: 1 }}>
             <Svg width={100} height={100}>
               <Path d="M 10 90 A 40 40 0 1 1 90 90" stroke="#f4a261" strokeWidth={8} fill="none" />
               <Circle cx={50} cy={50} r={12} fill="#16202f" stroke="#f4a261" strokeWidth={3} />
             </Svg>
           </View>
         );
       }`,
      'svg',
    );
    expect(r.c).toMatch(/static const float s_svg\d+_ops\[\]/); // baked tape (a fully static <Svg>)
    expect(r.c).toMatch(
      /er_node_set_vector_ops\(n\d+, s_svg\d+_ops, \d+, s_svg\d+_paints, \d+/,
    );
  });

  (CC ? it : it.skip)(
    `the generated C passes a C compiler syntax check (${CC || 'no cc found'})`,
    async () => {
      const r = await emitThermostat();
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        // -fsyntax-only: the struct (ERVectorGradient) + signature (er_node_set_vector_ops) are unconditional
        // in the engine headers, so this validates the codegen against the real API with no gradient flags.
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {
            encoding: 'utf8',
          },
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `a node pool smaller than the tree fails the C build (${CC || 'no cc found'})`,
    async () => {
      const r = await emitThermostat();
      const nodes = r.nodes + 1; // and the root
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-pool-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const check = pool =>
          spawnSync(
            CC,
            [
              '-fsyntax-only',
              `-DERUI_MAX_NODES=${pool}`,
              '-I',
              engineInc,
              '-I',
              engineCore,
              join(dir, 'app.gen.c'),
            ],
            {encoding: 'utf8'},
          );
        const tooSmall = check(nodes - 1);
        expect(tooSmall.status).not.toBe(0);
        expect(tooSmall.stderr).toContain(
          `raise ERUI_MAX_NODES to at least ${nodes}`,
        );
        expect(check(nodes).status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `an app past the default node pool fails a build that never set one (${CC || 'no cc found'})`,
    () => {
      const rows = Array.from({length: 520}, (_, i) => i).join(', ');
      const r = compileSource(
        `import {View, Text} from 'embedded-react';
         export function App() {
           return (<View>{[${rows}].map(n => <Text>{n}</Text>)}</View>);
         }`,
        'big',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-default-pool-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        // Only the public headers, and no -DERUI_MAX_NODES: er_scene.h's default applies.
        const res = spawnSync(
          CC,
          ['-fsyntax-only', '-I', engineInc, join(dir, 'app.gen.c')],
          {encoding: 'utf8'},
        );
        expect(res.status).not.toBe(0);
        expect(res.stderr).toContain(
          `raise ERUI_MAX_NODES to at least ${r.nodes + 1}`,
        );
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `reports a full list and a string cut short once each (${CC || 'no cc found'})`,
    () => {
      const {c} = compileSource(
        `import {useState} from 'react';
         import {Text, Pressable} from 'embedded-react';
         export function App() {
           const [n, setN] = useState(12345678);
           const [label, setLabel] = useState('');
           const [items, setItems] = useState([{t: ''}]);
           return (
             <Pressable onPress={() => { setLabel('n=' + n); setItems([...items, {t: 'n=' + n}]); }}>
               <Text>{label}</Text>
             </Pressable>
           );
         }`,
        'limits',
        {listCap: 2, listStrCap: 8},
      );
      // Lift the state and the handler out of the generated file and run the handler three times.
      const pragmas = c.match(/#if defined\(__GNUC__\)[\s\S]*?#endif\n/)[0];
      const state = c.match(
        /typedef struct\n\{\n[\s\S]*?static ErAppChanged s_changed;\n/,
      )[0];
      const handler = c.match(
        /static void er_handler_0\([^)]*\)\n\{[\s\S]*?\n\}\n/,
      )[0];
      const harness = `#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "er_scene.h"
${pragmas}${APP_LIMIT_DEFINITION}
${state}
static void app_update(void)
{
}

${handler}
int main(void)
{
    for (int i = 0; i < 3; i++)
    {
        er_handler_0(NULL, NULL, NULL);
    }
    printf("%d %s %s\\n", s_items_count, s_state.label, s_items[1].t);
    return 0;
}
`;
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-limits-'));
      try {
        writeFileSync(join(dir, 'limits.c'), harness);
        const exe = join(dir, 'limits.exe');
        const build = spawnSync(
          CC,
          [
            '-Wall',
            '-Werror',
            '-I',
            engineInc,
            join(dir, 'limits.c'),
            '-o',
            exe,
          ],
          {encoding: 'utf8'},
        );
        expect(build.stderr || '').toBe('');
        expect(build.status).toBe(0);
        const run = spawnSync(exe, [], {encoding: 'utf8'});
        expect(run.stdout).toBe('2 n=12345 n=12345\n');
        expect(run.stderr).toBe(
          "AOT: state 'label' cut short, raise ER_AOT_LIST_STR_CAP\n" +
            "AOT: field 't' of list 'items' cut short, raise ER_AOT_LIST_STR_CAP\n" +
            "AOT: list 'items' full, raise ER_AOT_LIST_CAP\n",
        );
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `a <Dial> app (state + animated value + onChange) passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      const r = compileSource(
        `import { useState } from 'react';
         import { View, Dial, useAnimatedValue } from 'embedded-react';
         export function App() {
           const [temp, setTemp] = useState(21);
           const level = useAnimatedValue(0);
           return (
             <View style={{ flex: 1 }}>
               <Dial value={temp} min={10} max={30} step={0.5} cap="round" knob="circle" adjustable
                     indicatorColor={temp > 25 ? '#ff4040' : '#ff8800'} onChange={(v) => setTemp(v)}
                     style={{ width: 200, height: 200 }} />
               <Dial value={level} max={100} segments={8} gapAngle={3}
                     indicatorGradient={{ type: 'conic', stops: [{ color: '#0000ff' }, { color: '#ff0000' }] }} />
             </View>
           );
         }`,
        'dial',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-dial-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `looped animations and their stop() pass the C syntax check (${CC || 'no cc found'})`,
    () => {
      // The starter's pulse, plus every other shape a loop chain emits: a counted loop's iteration counter
      // and completion call, a single animation's reset slot, and the stop() a dep-driven cleanup runs.
      const starter = readFileSync(
        join(root, 'create-embedded-react', 'template', 'App.jsx'),
        'utf8',
      );
      const loops = compileSource(
        `import { useState, useEffect } from 'react';
         import { View, Animated, useAnimatedValue } from 'embedded-react';
         export function App() {
           const [on, setOn] = useState(false);
           const [runs, setRuns] = useState(0);
           const a = useAnimatedValue(0);
           const b = useAnimatedValue(0);
           useEffect(() => {
             if (!on) return;
             const anim = Animated.loop(
               Animated.sequence([
                 Animated.timing(a, { toValue: 1, duration: 300 }),
                 Animated.delay(100),
                 Animated.timing(a, { toValue: 0, duration: 300 }),
               ]),
               { iterations: 3 },
             );
             anim.start(({ finished }) => setRuns(finished ? runs + 1 : runs));
             return () => anim.stop();
           }, [on]);
           useEffect(() => {
             Animated.loop(Animated.spring(b, { toValue: 1 }), { iterations: 2 }).start();
           }, []);
           return <View onPress={() => setOn(!on)} style={{ flex: 1, opacity: a, width: runs }} />;
         }`,
        'loops',
      );
      // Completion waits: a sequence's trailing delay stopped by a cleanup, a delay alone, a stop of a
      // composition nothing waits on (its slot is dropped), and a spring joined with a longer delay.
      const waits = compileSource(
        `import { useState, useEffect } from 'react';
         import { View, Animated, useAnimatedValue } from 'embedded-react';
         export function App() {
           const [n, setN] = useState(0);
           const a = useAnimatedValue(0);
           useEffect(() => {
             const pulse = Animated.sequence([Animated.timing(a, { toValue: 1 }), Animated.delay(300)]);
             pulse.start(({ finished }) => setN(finished ? 1 : 2));
             return () => pulse.stop();
           }, [n]);
           return (
             <View
               onPress={() => {
                 Animated.delay(500).start(() => setN(3));
                 Animated.parallel([Animated.timing(a, { toValue: 0 }), Animated.delay(900)]).stop();
                 Animated.parallel([Animated.spring(a, { toValue: 1 }), Animated.delay(400)]).start(() => setN(4));
               }}
               style={{ flex: 1, opacity: a, width: n }}
             />
           );
         }`,
        'waits',
      );
      expect(waits.c).toContain('app_wait_start(0, 500, er_donecb_0);');
      expect(waits.c).toContain('app_wait_detach(3);');
      expect(waits.c).toContain('app_wait_tick(dt_ms);');
      expect(waits.c).not.toContain('app_wait_detach(1);');
      expect(waits.c).toContain('app_wait_join(2, 400, 1, er_donecb_1);');
      for (const [name, r] of [
        [
          'starter',
          compileSource(starter, 'starter', {
            screen: {width: 240, height: 320},
            filename: 'template/App.jsx',
          }),
        ],
        ['loops', loops],
        ['waits', waits],
      ]) {
        const dir = mkdtempSync(join(tmpdir(), `er-aot-cc-${name}-`));
        try {
          writeFileSync(join(dir, 'app.gen.c'), r.c);
          writeFileSync(join(dir, 'app.gen.h'), r.h);
          const res = spawnSync(
            CC,
            [
              '-fsyntax-only',
              '-Wall',
              '-I',
              engineInc,
              '-I',
              engineCore,
              join(dir, 'app.gen.c'),
            ],
            {encoding: 'utf8'},
          );
          expect(res.stderr || '', name).toBe('');
          expect(res.status, name).toBe(0);
        } finally {
          rmSync(dir, {recursive: true, force: true});
        }
      }
    },
  );

  (CC ? it : it.skip)(
    `completion waits run independently, and a stop ends only its own (${CC || 'no cc found'})`,
    () => {
      // Runs the generated wait helpers: separate runs of one composition, a stop, and completions that start new
      // runs from inside a stop.
      const harness = `#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void (*ERAnimCompleteFn)(bool finished, void* user_data);
${APP_LIMIT_DEFINITION}
${printWaitDeclarations('app_wait_start( app_wait_detach(')}

static char s_log[256];
static void done_a(bool finished, void* user_data)
{
    (void)user_data;
    strcat(s_log, finished ? "A+" : "A-");
}
static void done_b(bool finished, void* user_data)
{
    (void)user_data;
    strcat(s_log, finished ? "B+" : "B-");
}
static void done_restart(bool finished, void* user_data)
{
    (void)user_data;
    strcat(s_log, finished ? "R+" : "R-");
    if (!finished)
    {
        app_wait_start(1, 100, done_b);
    }
}

int main(void)
{
    app_wait_start(0, 100, done_a);
    app_wait_start(0, 150, done_a);
    app_wait_tick(120);
    app_wait_tick(40);
    strcat(s_log, "|");

    app_wait_start(0, 100, done_a);
    app_wait_start(2, 100, done_b);
    app_wait_detach(0);
    app_wait_report(0);
    app_wait_tick(200);
    strcat(s_log, "|");

    app_wait_start(1, 100, done_restart);
    app_wait_detach(1);
    app_wait_report(1);
    app_wait_tick(200);
    strcat(s_log, "|");

    app_wait_start(1, 100, done_a);
    app_wait_detach(1);
    app_wait_start(1, 100, done_b);
    app_wait_report(1);
    app_wait_tick(200);
    strcat(s_log, "|");

    for (int i = 0; i < ER_AOT_MAX_WAITS; i++)
    {
        app_wait_start(3, 100, done_b);
    }
    app_wait_start(3, 100, done_a);
    strcat(s_log, "/");
    app_wait_tick(200);
    puts(s_log);
    return 0;
}
`;
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-waits-'));
      try {
        writeFileSync(join(dir, 'waits.c'), harness);
        const exe = join(dir, 'waits.exe');
        const build = spawnSync(
          CC,
          ['-Wall', '-Werror', join(dir, 'waits.c'), '-o', exe],
          {encoding: 'utf8'},
        );
        expect(build.stderr || '').toBe('');
        expect(build.status).toBe(0);
        const run = spawnSync(exe, [], {encoding: 'utf8'});
        // Both taps complete; the stop ends owner 0 only; a run started by the stop's completion, or between
        // detach and report, survives; a run that finds the table full ends at once, as not finished.
        expect(run.stdout.trim()).toBe(
          'A+A+|A-B+|R-B+|A-B+|A-/' + 'B+'.repeat(8),
        );
        expect(run.stderr).toBe(
          'AOT: wait table full, raise ER_AOT_MAX_WAITS\n',
        );
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `a joined run completes when its animation and its delay have both ended (${CC || 'no cc found'})`,
    () => {
      // Runs the join helpers, standing in for the engine: app_wait_part is what an animation's on_complete calls.
      const harness = `#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void (*ERAnimCompleteFn)(bool finished, void* user_data);
${APP_LIMIT_DEFINITION}
${printWaitDeclarations('app_wait_start( app_wait_join( app_wait_detach(')}

static char s_log[256];
static void done_a(bool finished, void* user_data)
{
    (void)user_data;
    strcat(s_log, finished ? "A+" : "A-");
}
static void done_b(bool finished, void* user_data)
{
    (void)user_data;
    strcat(s_log, finished ? "B+" : "B-");
}

int main(void)
{
    void* join = app_wait_join(0, 500, 1, done_a);
    app_wait_part(true, join);
    app_wait_tick(300);
    strcat(s_log, ".");
    app_wait_tick(250);
    strcat(s_log, "|");

    join = app_wait_join(0, 100, 1, done_a);
    app_wait_tick(200);
    strcat(s_log, ".");
    app_wait_part(true, join);
    strcat(s_log, "|");

    join = app_wait_join(0, 500, 1, done_a);
    app_wait_part(false, join);
    app_wait_tick(600);
    strcat(s_log, "|");

    join = app_wait_join(1, 500, 1, done_a);
    app_wait_detach(1);
    app_wait_part(false, join);
    strcat(s_log, ".");
    app_wait_report(1);
    app_wait_tick(600);
    strcat(s_log, "|");

    join = app_wait_join(1, 500, 1, done_a);
    app_wait_part(true, join);
    app_wait_detach(1);
    app_wait_report(1);
    app_wait_tick(600);
    strcat(s_log, "|");

    join = app_wait_join(0, 100, 2, done_a);
    app_wait_part(false, join);
    app_wait_part(true, join);
    app_wait_tick(200);
    strcat(s_log, "|");

    for (int i = 0; i < ER_AOT_MAX_WAITS; i++)
    {
        app_wait_start(3, 100, done_b);
    }
    join = app_wait_join(3, 100, 1, done_a);
    app_wait_part(true, join);
    strcat(s_log, join ? "?" : "/");
    app_wait_tick(200);
    puts(s_log);
    return 0;
}
`;
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-joins-'));
      try {
        writeFileSync(join(dir, 'joins.c'), harness);
        const exe = join(dir, 'joins.exe');
        const build = spawnSync(
          CC,
          ['-Wall', '-Werror', join(dir, 'joins.c'), '-o', exe],
          {encoding: 'utf8'},
        );
        expect(build.stderr || '').toBe('');
        expect(build.status).toBe(0);
        const run = spawnSync(exe, [], {encoding: 'utf8'});
        // Whichever ends last completes the run; an interruption or a stop ends it at once, as not finished, and
        // only once; a run kept until its last animation reports frees its wait, as every run above does (the
        // table then holds eight more); a run that finds the table full ends at once.
        expect(run.stdout.trim()).toBe(
          '.A+|.A+|A-|.A-|A-|A-|A-/' + 'B+'.repeat(8),
        );
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `helper names inside text do not pull in unused helpers (${CC || 'no cc found'})`,
    () => {
      // The scan for helper calls must not read string literals: this text names four helpers and the app
      // calls none of them.
      const r = compileSource(
        `import { useState, useEffect } from 'react';
         import { View, Text } from 'embedded-react';
         export function App() {
           const [n, setN] = useState(0);
           useEffect(() => { setInterval(() => setN(v => v + 1), 1000); }, []);
           return (
             <View style={{ flex: 1 }}>
               <Text>app_perf_now(</Text>
               <Text>{'app_date_now( app_floordiv64( er_timer_clear( ' + n}</Text>
             </View>
           );
         }`,
        'literal-helpers',
      );
      expect(r.c).not.toContain('static int64_t app_date_now(void)');
      expect(r.c).not.toContain('static void er_timer_clear(int id)');
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-literal-helpers-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `a timer cleared only from a mount effect's cleanup passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      // That cleanup is dropped (the app never unmounts), so nothing calls er_timer_clear — and a static
      // function nothing calls is a -Wall warning.
      const r = compileSource(
        `import { useState, useEffect } from 'react';
         import { View, Text } from 'embedded-react';
         export function App() {
           const [n, setN] = useState(0);
           useEffect(() => {
             const id = setInterval(() => setN(v => v + 1), 1000);
             return () => clearInterval(id);
           }, []);
           return (<View style={{ flex: 1 }}><Text>{n}</Text></View>);
         }`,
        'mount-timer',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-mount-timer-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `64-bit time (Date.now / performance.now) passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      // Every place a timestamp can go, under -Wall: a printf format that does not match int64_t on this
      // host, or a clock helper emitted but never called, would warn.
      const r = compileSource(
        `import { useState, useEffect, useRef } from 'react';
         import { View, Text, Pressable } from 'embedded-react';
         export function App() {
           const [t, setT] = useState(0);
           const [sec, setSec] = useState(0);
           const start = useRef(0);
           useEffect(() => {
             start.current = performance.now();
           }, []);
           useEffect(() => {
             const id = setInterval(() => setSec(Math.floor((performance.now() - start.current) / 1000) % 60), 1000);
             return () => clearInterval(id);
           }, [t]);
           return (
             <View style={{ flex: 1 }}>
               <Pressable onPress={() => { const now = Date.now(); if (now - t > 500) setT(now); }}>
                 <Text>{t}</Text>
               </Pressable>
               <Pressable onPress={() => setT(Date.now() || 0)}><Text>now</Text></Pressable>
               <Pressable onPress={() => setTimeout(() => setSec(0), t - Date.now())}><Text>later</Text></Pressable>
               <Text>{'up ' + Math.max(0, Math.abs(performance.now() - start.current)) + ' ms'}</Text>
               <Text>{Math.floor(Date.now() / 60000) % 60}</Text>
               <View style={{ width: Date.now() % 100, height: 4 }} />
             </View>
           );
         }`,
        'time',
      );
      expect(r.c).toContain(
        'static int64_t app_floordiv64(int64_t a, int64_t b)',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-time-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `the state-driven dimension rounder passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      // app_round_dim is the only helper the codegen writes into the file itself rather than calling from
      // the engine, and it is emitted only when something needs it — so nothing else here would notice it
      // failing to compile.
      const r = compileSource(
        `import { useState } from 'react';
         import { View, Text } from 'embedded-react';
         export function App() {
           const [n, setN] = useState(3);
           return (<View style={{ width: n * 0.8875, marginLeft: -n / 2 }}><Text>x</Text></View>);
         }`,
        'round',
      );
      expect(r.c).toContain('static int16_t app_round_dim(double v)');
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-round-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `early-returning useEffects pass the C syntax check (${CC || 'no cc found'})`,
    () => {
      // An early return hoists a mount effect out of er_app_build into a function of its own — a shape
      // nothing else emits, and one that needs its own forward declaration to compile.
      const r = compileSource(
        `import { useState, useEffect } from 'react';
         import { View, Text } from 'embedded-react';
         export function App() {
           const [page, setPage] = useState(0);
           const [t, setT] = useState(0);
           useEffect(() => { if (page > 1) return; setPage(1); }, []);
           useEffect(() => {
             if (page !== 2) return undefined;
             const id = setInterval(() => setT((v) => (v + 1) % 24), 90);
             return () => clearInterval(id);
           }, [page]);
           return (<View style={{ flex: 1 }}><Text>{t}</Text></View>);
         }`,
        'effret',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-effret-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `a PanResponder pager passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      // The responder lowering generates two callback SHAPES the rest of the codegen never emits — a
      // bool-returning ERResponderQueryFn and er_responder_query_set — plus a call to the engine's own
      // er_touch_active_count(). A regex can't tell whether those still match the engine headers.
      const r = compileSource(
        `import { useState, useRef } from 'react';
         import { View, Text, PanResponder } from 'embedded-react';
         export function App() {
           const [page, setPage] = useState(0);
           const [slide, setSlide] = useState(0);
           const at = useRef(0);
           const pan = useRef(PanResponder.create({
             onStartShouldSetPanResponder: () => true,
             onMoveShouldSetPanResponderCapture: (e, g) => Math.abs(g.dx) > 8,
             onPanResponderTerminationRequest: () => false,
             onPanResponderGrant: (e, g) => { at.current = g.x0; },
             onPanResponderMove: (e, g) => setSlide(Math.max(0, Math.min(240, at.current - g.dx))),
             onPanResponderRelease: (e, g) => setPage(g.vx < -0.4 || g.dx < -120 ? 1 : 0),
             onPanResponderTerminate: () => setSlide(0),
             onPanResponderReject: (e, g) => setSlide(g.numberActiveTouches),
           })).current;
           return (
             <View style={{ flex: 1, marginLeft: -slide }}>
               <Text>{page}</Text>
               <View style={{ width: 240, height: 240 }} {...pan.panHandlers} />
             </View>
           );
         }`,
        'pan',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-pan-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `a state-driven <Rect rx> passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      // A corner radius bounded by a state-driven side clamps in C (fminf/fmaxf), so this is the one
      // rounded-rect path a regex test can't vouch for — it needs math.h to actually be included.
      const r = compileSource(
        `import { useState, useRef } from 'react';
         import { View, Svg, Rect, Pressable, updateVector } from 'embedded-react';
         export function App() {
           const [pct, setPct] = useState(40);
           const bar = useRef(null);
           return (
             <View style={{ flex: 1 }}>
               <Pressable onPress={() => setPct(pct + 10)}>
                 <Svg width={200} height={24}>
                   <Rect x={0} y={0} width={200} height={24} rx={12} fill="#16202f" />
                   <Rect x={0} y={0} width={pct * 2} height={24} rx={12} fill="#f4a261" />
                 </Svg>
               </Pressable>
               <Pressable onPress={() => updateVector(bar, [{ rect: [0, 0, pct * 2, 24, 12, 12], fill: '#f4a261' }])}>
                 <Svg ref={bar} width={200} height={24} />
               </Pressable>
             </View>
           );
         }`,
        'progress',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-rrect-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `a <TouchableOpacity> passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      // The press feedback is the one place the codegen writes an ERAnimConfig NOBODY asked for — it is
      // synthesized from the tag, not lowered from an Animated.* call — and it wraps the app's own handler
      // by calling it through the ERNodeEventFn signature. A regex sees neither of those go stale.
      const r = compileSource(
        `import { useState } from 'react';
         import { View, Text, TouchableOpacity } from 'embedded-react';
         export function App() {
           const [n, setN] = useState(0);
           return (
             <View style={{ flex: 1 }}>
               <TouchableOpacity activeOpacity={0.4} style={{ padding: 8, opacity: 0.9 }}
                                 onPress={() => setN(n + 1)} onPressIn={() => setN(0)}>
                 <Text>{n}</Text>
               </TouchableOpacity>
             </View>
           );
         }`,
        'touchable',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-touchable-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );
  (CC ? it : it.skip)(
    `saturating whole-number math passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      // Every checked-math helper, int and 64-bit, under -Wall: one emitted but never called would warn,
      // and so would a printf format that no longer matches a widened value.
      const r = compileSource(
        `import { useState, useRef } from 'react';
         import { View, Text, Pressable } from 'embedded-react';
         const DAY_MS = 86400000;
         export function App() {
           const [n, setN] = useState(0);
           const [t, setT] = useState(0);
           const acc = useRef(0);
           return (
             <View style={{ flex: 1 }}>
               <Pressable onPress={() => { setN(-(n * 3 - 1) + 2); acc.current += n; acc.current++; }}>
                 <Text>{n}</Text>
               </Pressable>
               <Pressable onPress={() => setT(-(Date.now() * 2 - 30 * DAY_MS) + n * DAY_MS)}><Text>{t}</Text></Pressable>
               <Text>{Math.abs(Date.now() - t)}</Text>
             </View>
           );
         }`,
        'checked',
      );
      for (const h of [
        'app_add',
        'app_sub',
        'app_mul',
        'app_neg',
        'app_add64',
        'app_sub64',
        'app_mul64',
        'app_neg64',
        'app_abs64',
      ])
        expect(r.c).toMatch(new RegExp(`static int(64_t)? ${h}\\(`));
      expect(r.c).toContain('#include <limits.h>');
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-checked-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            '-Wextra',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `integer division and float conversion pass the C syntax check (${CC || 'no cc found'})`,
    () => {
      // Every division and conversion helper under -Wall -Wextra. app_roundf needs <math.h> though the app
      // itself calls no libm function, so a missing include only shows up here.
      const r = compileSource(
        `import { useState, useRef } from 'react';
         import { View, Text, Pressable, Svg, updateVector } from 'embedded-react';
         export function App() {
           const [n, setN] = useState(7);
           const [f, setF] = useState(0.5);
           const q = useRef(9);
           const bar = useRef(null);
           return (
             <View style={{ flex: 1, opacity: f }}>
               <Pressable onPress={() => { setN(n % q.current); q.current /= n; q.current %= f; setN(f * 3); setTimeout(() => setF(0.25), f * 1000); updateVector(bar, [{ rect: [0, 0, f * 100, 10], fill: '#ffffff' }], [0, 0, f * 100, 10]); }}>
                 <Text>{Math.round(f)}</Text>
                 <Text>{f % 2}</Text>
                 <Text>{Math.floor(n / q.current)}</Text>
                 <Text>{Math.ceil(n / q.current)}</Text>
                 <Text>{Math.round(n / q.current)}</Text>
                 <Text>{Math.trunc(n / q.current)}</Text>
               </Pressable>
               <Svg ref={bar} width={100} height={10} />
             </View>
           );
         }`,
        'divconv',
      );
      for (const h of [
        'static int app_mod(',
        'static int app_div(',
        'static int app_floordiv(',
        'static int app_ceildiv(',
        'static int app_rounddiv(',
        'static int app_f2i(',
        'static float app_roundf(',
        'static const char* app_ftoa(',
        'static uint8_t app_opacity(',
        'static int app_delay_msf(',
        'static void app_vector_dirty(',
        'fmodf(',
      ])
        expect(r.c).toContain(h);
      expect(r.c).toContain('#include <math.h>');
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-divconv-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            '-Wextra',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `state that nothing on screen reads passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      // app_update is emitted only when a node reads state, so every body that would call it has to leave
      // the call out too: an undeclared function is an error in C99 and later.
      const r = compileSource(
        `import { useState, useEffect } from 'react';
         import { View, Text, Pressable, Switch, Animated, useAnimatedValue } from 'embedded-react';
         export function App() {
           const [n, setN] = useState(0);
           const [items, setItems] = useState([{w: 1}]);
           const a = useAnimatedValue(0);
           useEffect(() => { setN(1); }, []);
           useEffect(() => { if (n > 5) return; setN(2); }, []);
           useEffect(() => { setInterval(() => setN((v) => v + 1), 1000); }, []);
           return (
             <View style={{ flex: 1 }}>
               <Pressable onPress={() => { setN(n + 1); setItems([...items, {w: 2}]); Animated.timing(a, {toValue: 1, duration: 300}).start(() => setN(0)); }}>
                 <Text>x</Text>
               </Pressable>
               <Switch value={true} onValueChange={() => setN(n - 1)} />
             </View>
           );
         }`,
        'unread',
      );
      expect(r.c).not.toContain('app_update');
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-unread-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `concatenated text passes the C syntax check with -Wformat (${CC || 'no cc found'})`,
    () => {
      // A `+` chain over strings used to be typed as arithmetic, so it emitted "%d" over C's `+` on two
      // char pointers — invalid C the AOT itself accepted. -Wformat is what proves the format string and
      // the argument list agree, which no regex over the generated text can.
      const r = compileSource(
        `import { useState } from 'react';
         import { View, Text, TextInput } from 'embedded-react';
         const PAGES = 4;
         export function App() {
           const [page, setPage] = useState(0);
           const [hit, setHit] = useState('none');
           const [ratio, setRatio] = useState(0.5);
           const [label, setLabel] = useState('');
           const [on, setOn] = useState(false);
           return (
             <View style={{ flex: 1 }}>
               <Text>{'render-check ' + (page + 1) + '/' + PAGES}</Text>
               <Text>{'hit: ' + hit}</Text>
               <Text onPress={() => setLabel('page ' + page)}>{ratio + '% of ' + PAGES}</Text>
               {/* A boolean lowers to %s over a ternary of string literals — a pairing only -Wformat checks. */}
               <Text>{'on: ' + on + ' hot: ' + (page > 2)}</Text>
               <Text>{'nul: ' + label + null}</Text>
               {/* Self-referential setter: builds in a temporary, so snprintf never reads its own target. */}
               <Text onPress={() => setLabel(label + '!')}>{label}</Text>
               <TextInput value={'#' + page} />
             </View>
           );
         }`,
        'concat',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-concat-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-Wall',
            '-Wformat',
            ...GCC_FORMAT_FLAGS,
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );
});
