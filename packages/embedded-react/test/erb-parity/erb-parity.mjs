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

// erb-parity.mjs — the package output tested against the C output (`npm run erb:parity [filter]`).
//
// Each scenario is compiled twice, to app.gen.c and to a Flow B package. tools/erb-parity builds a headless
// host around each (the C app, and the Flow B runtime loading the package), both run the scenario's taps
// through the same engine and software backend, and their last frames must match pixel for pixel. Each
// package's resource report is printed as it is built.
//
// A scenario the package output can't lower yet is skipped with the reason, so the list shows how far the
// package output has come; any other failure fails the run. Frames that differ are written as PNGs
// (C, VM and a diff) under the build folder.
//
// Needs CMake and a C compiler; no display.

import {spawnSync} from 'node:child_process';
import {
  existsSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {PNG} from 'pngjs';

import {
  bakeSvgArtifacts,
  compilePackage,
  compileSource,
  packPackage,
  resourceReport,
} from '../../aot/compile.mts';
import {bakeAssetPackBytes, bakeAssets} from '../../assets/index.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '../../../..');
const HOST_DIR = join(ROOT, 'tools/erb-parity');
const BUILD_DIR = process.env.ERB_PARITY_BUILD_DIR || join(HOST_DIR, 'build');
const APPS_DIR = join(BUILD_DIR, 'apps');
const OUT_DIR = join(BUILD_DIR, 'out');
const EXE = process.platform === 'win32' ? '.exe' : '';

const fixture = name => join(HERE, 'fixtures', `${name}.jsx`);
const demo = name => join(ROOT, 'demos', name, 'App.jsx');

// `taps` is "x,y x,y ...", the same for both hosts; "x,y>x2,y2" is a drag. `settle` is how many ms the hosts run after the last tap
// before they take the frame (500 when it is left out), so a scenario can sample an animation as it runs.
const SCENARIOS = [
  {name: 'layout', file: fixture('layout'), screen: [240, 320]},
  {name: 'text', file: fixture('text'), screen: [240, 320]},
  // Focusing the TextInput brings up the engine's keyboard, with no app code involved.
  {
    name: 'text-focused',
    file: fixture('text'),
    screen: [240, 320],
    taps: '120,240',
  },
  {name: 'style', file: fixture('style'), screen: [240, 320]},
  {name: 'spans', file: fixture('spans'), screen: [240, 320]},
  {name: 'ref', file: fixture('ref'), screen: [240, 320]},
  {name: 'inputs', file: fixture('inputs'), screen: [240, 320]},
  // The switch, then "hi" typed on the engine's keyboard.
  {
    name: 'inputs-typed',
    file: fixture('inputs'),
    screen: [240, 320],
    taps: '36,103 120,30 140,247 177,217',
  },
  {name: 'counter', file: fixture('counter'), screen: [240, 320]},
  // Four presses of +, one of -, then the toggle.
  {
    name: 'counter-taps',
    file: fixture('counter'),
    screen: [240, 320],
    taps: '122,142 122,142 122,142 122,142 43,142 200,142',
  },
  // The toggle twice: back off, with the interned mood set to its other value.
  {
    name: 'counter-mood',
    file: fixture('counter'),
    screen: [240, 320],
    taps: '200,142 200,142',
  },
  // The mount effect's interval ticks through the frames the host settles for.
  {name: 'timers', file: fixture('timers'), screen: [240, 320]},
  // Two timeouts, then an interval started and cleared before the last frame.
  {
    name: 'timers-taps',
    file: fixture('timers'),
    screen: [240, 320],
    taps: '45,30 120,30 195,30',
  },
  // A tap's interval left running, and a timeout started again over its own.
  {
    name: 'timers-running',
    file: fixture('timers'),
    screen: [240, 320],
    taps: '120,30 45,30 45,30',
  },
  {name: 'effects', file: fixture('effects'), screen: [240, 320]},
  // + twice, then the mode: the mode effect's write shows when the + effect's timeout fires.
  {
    name: 'effects-taps',
    file: fixture('effects'),
    screen: [240, 320],
    taps: '45,30 45,30 120,30',
  },
  // Taps on nothing until the mount's timeout has fired, then the mode: its effect's write waits for the next
  // update, which never comes.
  {
    name: 'effects-stale',
    file: fixture('effects'),
    screen: [240, 320],
    taps: '200,300 200,300 200,300 120,30',
  },
  // The mode, then +: the + shows the mode effect's write, and its timeout the + effect's.
  {
    name: 'effects-mode',
    file: fixture('effects'),
    screen: [240, 320],
    taps: '120,30 45,30',
  },
  {name: 'lists', file: fixture('lists'), screen: [240, 320]},
  // Two adds, a press on the new last row, then the dots twice (two dots each).
  {
    name: 'lists-adds',
    file: fixture('lists'),
    screen: [240, 320],
    taps: '28,20 28,20 20,108 28,148 28,148',
  },
  // Drop, a row's press, keep, add, a row's press, then cut to none and add again.
  {
    name: 'lists-removes',
    file: fixture('lists'),
    screen: [240, 320],
    taps: '72,20 20,76 116,20 28,20 20,60 160,20 28,20',
  },
  // Presses on two rows, cut by a negative end, clear, the dots (now where the first row was), then adds and a drop.
  {
    name: 'lists-clear',
    file: fixture('lists'),
    screen: [240, 320],
    taps: '20,92 20,76 160,20 204,20 20,60 28,20 28,20 72,20',
  },
  // Buttons: row 1 fade, spring, sequence, counted loop, stop; row 2 join, wait, stagger, decay, sleep; row 3 a
  // sequence that ends on a delay, an endless chain, an empty sequence, a join stopped at once, a looped sequence.
  {name: 'animations', file: fixture('animations'), screen: [240, 320]},
  {
    name: 'anim-fade',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '28,23',
    settle: 200,
  },
  // The second fade cancels the first, whose completion runs as not finished; the third starts from the second's.
  {
    name: 'anim-fade-again',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '28,23 28,23',
    settle: 600,
  },
  {
    name: 'anim-spring',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '74,23',
    settle: 150,
  },
  {
    name: 'anim-seq',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '120,23',
    settle: 250,
  },
  {
    name: 'anim-seq-done',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '120,23',
    settle: 700,
  },
  {
    name: 'anim-loop',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '166,23',
    settle: 220,
  },
  {
    name: 'anim-loop-done',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '166,23',
    settle: 600,
  },
  // A fade and a sequence, both stopped mid-way.
  {
    name: 'anim-stop',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '28,23 120,23 212,23',
    settle: 600,
  },
  {
    name: 'anim-join',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '28,59',
    settle: 500,
  },
  {
    name: 'anim-join-done',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '28,59',
    settle: 900,
  },
  // The wait's timing, then the stagger cuts it: its completion runs as not finished, so says nothing.
  {
    name: 'anim-wait',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '74,59',
    settle: 300,
  },
  {
    name: 'anim-wait-done',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '74,59',
    settle: 600,
  },
  {
    name: 'anim-stagger',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '120,59',
    settle: 150,
  },
  {
    name: 'anim-decay',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '166,59',
    settle: 200,
  },
  {
    name: 'anim-sleep',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '212,59',
    settle: 400,
  },
  {
    name: 'anim-held',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '28,95',
    settle: 500,
  },
  // The endless chain, an empty sequence's completion at once, and a join stopped as it starts.
  {
    name: 'anim-ever',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '74,95 120,95 166,95',
    settle: 400,
  },
  {
    name: 'anim-twice',
    file: fixture('animations'),
    screen: [240, 320],
    taps: '212,95',
    settle: 420,
  },
  {name: 'interpolate', file: fixture('interpolate'), screen: [240, 320]},
  {
    name: 'interpolate-spin',
    file: fixture('interpolate'),
    screen: [240, 320],
    settle: 250,
  },
  // A press of each <TouchableOpacity>, the frame taken while the second fades back.
  {
    name: 'interpolate-press',
    file: fixture('interpolate'),
    screen: [240, 320],
    taps: '80,174 80,218',
    settle: 60,
  },
  {
    name: 'interpolate-pressed',
    file: fixture('interpolate'),
    screen: [240, 320],
    taps: '80,174 80,174',
    settle: 500,
  },
  {name: 'drag', file: fixture('drag'), screen: [240, 320]},
  // A tap on the knob: a grant and a release with no travel.
  {name: 'drag-tap', file: fixture('drag'), screen: [240, 320], taps: '50,86'},
  // The knob dragged right, then back from where it landed.
  {
    name: 'drag-knob',
    file: fixture('drag'),
    screen: [240, 320],
    taps: '50,86>130,96 130,86>70,80',
  },
  // The pad, claimed once the finger is past its slop, so its travel starts from there.
  {
    name: 'drag-pad',
    file: fixture('drag'),
    screen: [240, 320],
    taps: '40,154>200,150',
  },
  {name: 'svg', file: fixture('svg'), screen: [240, 320]},
  // The arc twice, then the toggle (a wider stroke and the gradient) and the mode (a stop's and a stroke's color).
  {
    name: 'svg-state',
    file: fixture('svg'),
    screen: [240, 320],
    taps: '50,120 50,120 33,195 91,195',
  },
  // updateVector three times with a hint in ints, then twice with one in floats.
  {
    name: 'svg-update',
    file: fixture('svg'),
    screen: [240, 320],
    taps: '149,195 149,195 149,195 207,195 207,195',
  },
  // Two rows added, then the toggle redraws every row. Four rows keep the drawings within the host's vector nodes.
  {
    name: 'svg-rows',
    file: fixture('svg-rows'),
    screen: [240, 320],
    listCap: 4,
    taps: '33,23 33,23 91,23',
  },
  // The app's own keyboard, brought up by focusing the input.
  {
    name: 'keyboard',
    file: fixture('keyboard'),
    screen: [240, 320],
    taps: '120,30',
  },
  // "ab", the shift layer (its key lit), "C", a backspace, "E" and a space.
  {
    name: 'keyboard-typed',
    file: fixture('keyboard'),
    screen: [240, 320],
    taps: '120,30 24,245 72,245 24,275 120,245 216,275 216,245 120,275',
  },
  // Then done, which puts the keyboard away.
  {
    name: 'keyboard-done',
    file: fixture('keyboard'),
    screen: [240, 320],
    taps: '120,30 24,245 72,245 120,305',
  },
  {
    name: 'keyboard-look',
    file: fixture('keyboard-look'),
    screen: [240, 320],
    taps: '120,30',
  },
  {name: 'watch-face', file: demo('watch-face'), screen: [240, 280]},
  {name: 'thermostat', file: demo('thermostat'), screen: [320, 480]},
  // + twice, then Heat and Auto (Auto turns on the dial's gradient).
  {
    name: 'thermostat-modes',
    file: demo('thermostat'),
    screen: [320, 480],
    taps: '280,341 280,341 124,395 197,395',
  },
  // A tap on the dial's track sets the temperature, through onValueChange.
  {
    name: 'thermostat-dial',
    file: demo('thermostat'),
    screen: [320, 480],
    taps: '60,200',
  },
  // The cog opens the settings sheet.
  {
    name: 'thermostat-settings',
    file: demo('thermostat'),
    screen: [320, 480],
    taps: '290,32',
  },
  // Settings, then °C, then close: the main screen in Celsius.
  {
    name: 'thermostat-units',
    file: demo('thermostat'),
    screen: [320, 480],
    taps: '290,32 222,277 266,199',
  },
];

// What the package output refuses until it lowers it.
const NOT_YET =
  /the package output (doesn't support|has no engine prop|can't read)/;

/** Runs a command, throwing with its output when it fails. */
function run(command, args) {
  const result = spawnSync(command, args, {encoding: 'utf8'});
  if (result.status !== 0) {
    throw new Error(
      `${command} ${args.join(' ')} failed:\n${result.stdout || ''}${result.stderr || ''}`,
    );
  }
  return result;
}

/** Writes an ARGB8888 frame as a PNG. */
function writePng(path, pixels, [width, height]) {
  const png = new PNG({width, height});
  for (let index = 0; index < width * height; index++) {
    const argb = pixels[index];
    png.data[index * 4] = (argb >>> 16) & 0xff;
    png.data[index * 4 + 1] = (argb >>> 8) & 0xff;
    png.data[index * 4 + 2] = argb & 0xff;
    png.data[index * 4 + 3] = 0xff;
  }
  writeFileSync(path, PNG.sync.write(png));
}

/** Reads a host's frame. */
const readFrame = path => {
  const bytes = readFileSync(path);
  return new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.length / 4);
};

/**
 * Compiles a scenario both ways: the C output into its own folder, the package beside it.
 *
 * @returns The C output's folder, the package's path, and its resource report.
 */
async function compileScenario(scenario) {
  const src = readFileSync(scenario.file, 'utf8');
  const sourceDir = dirname(scenario.file);
  const options = {
    filename: scenario.file,
    screen: {width: scenario.screen[0], height: scenario.screen[1]},
    listCap: scenario.listCap,
    svgArtifacts: await bakeSvgArtifacts(src, sourceDir),
  };
  const images = result =>
    result.images.map(image => ({
      name: image.name,
      path: resolve(sourceDir, image.importPath),
    }));

  // The package first: one that can't be lowered yet skips the scenario before anything is built.
  const packaged = compilePackage(src, scenario.name, options);
  const packageImages = images(packaged);
  const file = packPackage(
    packaged,
    packageImages.length
      ? bakeAssetPackBytes({images: packageImages})
      : undefined,
  );
  const packagePath = join(BUILD_DIR, `${scenario.name}.erpkg`);
  writeFileSync(packagePath, file.bytes);

  const c = compileSource(src, scenario.name, options);
  const appDir = join(APPS_DIR, scenario.name);
  mkdirSync(appDir, {recursive: true});
  writeFileSync(join(appDir, 'app.gen.c'), c.c);
  writeFileSync(join(appDir, 'app.gen.h'), c.h);
  bakeAssets({images: images(c), fonts: [], outDir: appDir});
  return {appDir, packagePath, report: resourceReport(packaged, file)};
}

/**
 * Builds both hosts, the C one around the app in appDir. Its old build is removed first: some makes compare
 * timestamps to the second, and would keep a host built moments ago around another app.
 */
function buildHosts(appDir) {
  rmSync(join(BUILD_DIR, 'CMakeFiles', 'erb-parity-c.dir'), {
    recursive: true,
    force: true,
  });
  rmSync(host('erb-parity-c'), {force: true});
  run('cmake', [
    '-S',
    HOST_DIR,
    '-B',
    BUILD_DIR,
    '-DCMAKE_BUILD_TYPE=Release',
    `-DERB_PARITY_APP_DIR=${appDir}`,
  ]);
  run('cmake', ['--build', BUILD_DIR, '-j']);
}

/** A built host's path; a multi-config generator puts it under the build type. */
function host(name) {
  const direct = join(BUILD_DIR, `${name}${EXE}`);
  return existsSync(direct)
    ? direct
    : join(BUILD_DIR, 'Release', `${name}${EXE}`);
}

/**
 * Runs both hosts and compares their frames.
 *
 * @returns null when they match, or what differs.
 */
function compareScenario(scenario, packagePath) {
  const [width, height] = scenario.screen;
  const cFrame = join(OUT_DIR, `${scenario.name}.c.raw`);
  const vmFrame = join(OUT_DIR, `${scenario.name}.vm.raw`);
  const taps = scenario.taps || '';
  const settle = String(scenario.settle ?? 500);
  run(host('erb-parity-c'), [
    cFrame,
    String(width),
    String(height),
    taps,
    settle,
  ]);
  run(host('erb-parity-vm'), [
    vmFrame,
    String(width),
    String(height),
    packagePath,
    taps,
    settle,
  ]);

  const c = readFrame(cFrame);
  const vm = readFrame(vmFrame);
  const diff = new Uint32Array(c.length);
  let differing = 0;
  for (let index = 0; index < c.length; index++) {
    if (c[index] !== vm[index]) {
      differing++;
      diff[index] = 0xffff00ff;
    }
  }
  if (!differing) {
    return null;
  }

  writePng(join(OUT_DIR, `${scenario.name}.c.png`), c, scenario.screen);
  writePng(join(OUT_DIR, `${scenario.name}.vm.png`), vm, scenario.screen);
  writePng(join(OUT_DIR, `${scenario.name}.diff.png`), diff, scenario.screen);
  return `${differing} of ${c.length} pixels differ; see ${join(OUT_DIR, scenario.name)}.{c,vm,diff}.png`;
}

const filter = process.argv[2];
const scenarios = filter
  ? SCENARIOS.filter(scenario => scenario.name.includes(filter))
  : SCENARIOS;
if (!scenarios.length) {
  console.error(
    `No scenarios match "${filter}". Known: ${SCENARIOS.map(scenario => scenario.name).join(', ')}`,
  );
  process.exit(2);
}

for (const dir of [OUT_DIR, APPS_DIR]) {
  rmSync(dir, {recursive: true, force: true});
  mkdirSync(dir, {recursive: true});
}

let failed = 0;
let passed = 0;
for (const scenario of scenarios) {
  const label = `${scenario.name} @ ${scenario.screen.join('x')}${scenario.taps ? ` taps[${scenario.taps}]` : ''}${scenario.settle !== undefined ? ` +${scenario.settle}ms` : ''}`;
  try {
    const {appDir, packagePath, report} = await compileScenario(scenario);
    buildHosts(appDir);
    const problem = compareScenario(scenario, packagePath);
    if (problem) {
      failed++;
      console.log(`✗ ${label}: ${problem}`);
    } else {
      passed++;
      console.log(`✓ ${label}`);
    }
    console.log(report.join('\n'));
  } catch (error) {
    const message = error?.message ?? String(error);
    if (NOT_YET.test(message)) {
      console.log(
        `- ${label}: skipped, ${message.split('\n')[0].replace(/^AOT: /, '')}`,
      );
    } else {
      failed++;
      console.log(`✗ ${label}: ${message}`);
    }
  }
}

console.log(
  `\n${passed} matched, ${failed} failed, ${scenarios.length - passed - failed} skipped`,
);
process.exit(failed ? 1 : 0);
