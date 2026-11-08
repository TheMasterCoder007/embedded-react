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

// erb-frames.mts — Flow B packages tested against golden images (`npm run erb:frames [filter]`).
//
// Each scenario is compiled to a package. tools/erb-frames builds a headless host around the Flow B runtime, which
// runs the scenario's taps through the engine and the software backend, and its last frame must match the scenario's
// golden image in golden/, as `npm run erb:frames:record [filter]` last recorded it. Each channel may be off by one:
// an image is recorded on one machine and checked on others, whose float rounding differs. Each package's resource
// report is printed as it is built.
//
// The apps are in fixtures/: small ones that each exercise a feature and never change, so a frame that differs means
// the compiler, runtime or engine changed. A new feature gets a new app. The demos stay out of this, since they change.
//
// Frames that differ are written as PNGs (frame, golden and a diff) under the build folder.
//
// Needs CMake and a C compiler; no display.

import {spawnSync} from 'node:child_process';
import type {SpawnSyncReturns} from 'node:child_process';
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
  packPackage,
  resourceReport,
} from '../../aot/compile.mts';
import {bakeAssetPackBytes} from '../../assets/index.mjs';
import type {Screen} from '../../aot/compiler/options.mts';

/** A panel size, in pixels. */
type Size = [width: number, height: number];

/** One app run through scripted taps, ending on a frame that has a golden image. */
interface Scenario {
  name: string;
  file: string;
  screen: Size;
  /** The sizes the package keeps layouts for; by default, `screen`. */
  panels?: Size[];
  /** Build with no sizes named. */
  everySize?: boolean;
  listCap?: number;
  taps?: string;
  settle?: number;
  release?: boolean;
}

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '../../../..');
const HOST_DIR = join(ROOT, 'tools/erb-frames');
const BUILD_DIR = process.env.ERB_FRAMES_BUILD_DIR || join(HOST_DIR, 'build');
const OUT_DIR = join(BUILD_DIR, 'out');
const GOLDEN_DIR = join(HERE, 'golden');
// How far a channel may be from the golden image: what a machine's float rounding moves it by.
const GOLDEN_TOLERANCE = 1;
const EXE_SUFFIX = process.platform === 'win32' ? '.exe' : '';
// The panel most scenarios run at.
const PORTRAIT: Size = [240, 320];
// Opaque black, what a golden pixel's color is ORed into, since a PNG here keeps no alpha.
const OPAQUE_BLACK = 0xff000000;
// Magenta, marking a differing pixel in a diff frame.
const DIFF_MARK = 0xffff00ff;

const fixture = (name: string): string => join(HERE, 'fixtures', `${name}.tsx`);
// `everySize` builds the package with no sizes named, as `embedded-react build --aot` does, and runs that one package
// at each size.
const everySize = (name: string, file: string, sizes: Size[]): Scenario[] =>
  sizes.map(screen => ({
    name: `${name}-${screen.join('x')}`,
    file,
    screen,
    everySize: true,
  }));

// `taps` is "x,y x,y ..."; "x,y>x2,y2" is a drag. `settle` is how many ms the host runs after the last tap before it
// takes the frame (500 when it is left out), so a scenario can sample an animation as it runs. `release` builds the
// package for release, without its debug table.
const SCENARIOS: Scenario[] = [
  ...everySize('responsive', fixture('responsive'), [
    [240, 320],
    [320, 240],
    [500, 300],
    [333, 333],
    [641, 211],
  ]),
  {name: 'layout', file: fixture('layout'), screen: PORTRAIT},
  {name: 'text', file: fixture('text'), screen: PORTRAIT},
  {name: 'strings', file: fixture('strings'), screen: PORTRAIT},
  // + twice to 22.5, then - to 22.25: halves that toFixed rounds up, in text, a local and the list.
  {
    name: 'strings-taps',
    file: fixture('strings'),
    screen: PORTRAIT,
    taps: '112,177 112,177 40,177',
  },
  // Focusing the TextInput brings up the engine's keyboard, with no app code involved.
  {
    name: 'text-focused',
    file: fixture('text'),
    screen: PORTRAIT,
    taps: '120,240',
  },
  {name: 'style', file: fixture('style'), screen: PORTRAIT},
  {name: 'spans', file: fixture('spans'), screen: PORTRAIT},
  {name: 'ref', file: fixture('ref'), screen: PORTRAIT},
  {name: 'inputs', file: fixture('inputs'), screen: PORTRAIT},
  // The switch, then "hi" typed on the engine's keyboard.
  {
    name: 'inputs-typed',
    file: fixture('inputs'),
    screen: PORTRAIT,
    taps: '36,103 120,30 140,247 177,217',
  },
  {name: 'counter', file: fixture('counter'), screen: PORTRAIT},
  // Four presses of +, one of -, then the toggle.
  {
    name: 'counter-taps',
    file: fixture('counter'),
    screen: PORTRAIT,
    taps: '122,142 122,142 122,142 122,142 43,142 200,142',
  },
  {
    name: 'counter-taps-release',
    file: fixture('counter'),
    screen: PORTRAIT,
    taps: '122,142 122,142 122,142 122,142 43,142 200,142',
    release: true,
  },
  // The toggle twice: back off, with the interned mood set to its other value.
  {
    name: 'counter-mood',
    file: fixture('counter'),
    screen: PORTRAIT,
    taps: '200,142 200,142',
  },
  {name: 'branches', file: fixture('branches'), screen: PORTRAIT, listCap: 6},
  // + twice builds the home screen's badge, then the screen's own bump.
  {
    name: 'branches-home',
    file: fixture('branches'),
    screen: PORTRAIT,
    listCap: 6,
    taps: '38,56 38,56 44,128',
  },
  // Two adds while the list isn't built, the list screen, then one more add.
  {
    name: 'branches-list',
    file: fixture('branches'),
    screen: PORTRAIT,
    listCap: 6,
    taps: '104,56 104,56 104,22 104,56',
  },
  // + once, the third screen, then dark three times.
  {
    name: 'branches-more',
    file: fixture('branches'),
    screen: PORTRAIT,
    listCap: 6,
    taps: '38,56 170,22 170,56 170,56 170,56',
  },
  // + twice, the list with an add, then home again: built anew, with its badge and none of the list.
  {
    name: 'branches-back',
    file: fixture('branches'),
    screen: PORTRAIT,
    listCap: 6,
    taps: '38,56 38,56 104,22 104,56 38,22',
  },
  // The mount effect's interval ticks through the frames the host settles for.
  {name: 'timers', file: fixture('timers'), screen: PORTRAIT},
  // Two timeouts, then an interval started and cleared before the last frame.
  {
    name: 'timers-taps',
    file: fixture('timers'),
    screen: PORTRAIT,
    taps: '45,30 120,30 195,30',
  },
  // A tap's interval left running, and a timeout started again over its own.
  {
    name: 'timers-running',
    file: fixture('timers'),
    screen: PORTRAIT,
    taps: '120,30 45,30 45,30',
  },
  {name: 'effects', file: fixture('effects'), screen: PORTRAIT},
  // + twice, then the mode: the mode effect's write shows when the + effect's timeout fires.
  {
    name: 'effects-taps',
    file: fixture('effects'),
    screen: PORTRAIT,
    taps: '45,30 45,30 120,30',
  },
  // Taps on nothing until the mount's timeout has fired, then the mode: its effect's write waits for the next
  // update, which never comes.
  {
    name: 'effects-stale',
    file: fixture('effects'),
    screen: PORTRAIT,
    taps: '200,300 200,300 200,300 120,30',
  },
  // The mode, then +: the + shows the mode effect's write, and its timeout the + effect's.
  {
    name: 'effects-mode',
    file: fixture('effects'),
    screen: PORTRAIT,
    taps: '120,30 45,30',
  },
  {name: 'lists', file: fixture('lists'), screen: PORTRAIT},
  // Two adds, a press on the new last row, then the dots twice (two dots each).
  {
    name: 'lists-adds',
    file: fixture('lists'),
    screen: PORTRAIT,
    taps: '28,20 28,20 20,108 28,148 28,148',
  },
  // Drop, a row's press, keep, add, a row's press, then cut to none and add again.
  {
    name: 'lists-removes',
    file: fixture('lists'),
    screen: PORTRAIT,
    taps: '72,20 20,76 116,20 28,20 20,60 160,20 28,20',
  },
  // Presses on two rows, cut by a negative end, clear, the dots (now where the first row was), then adds and a drop.
  {
    name: 'lists-clear',
    file: fixture('lists'),
    screen: PORTRAIT,
    taps: '20,92 20,76 160,20 204,20 20,60 28,20 28,20 72,20',
  },
  // Buttons: row 1 fade, spring, sequence, counted loop, stop; row 2 join, wait, stagger, decay, sleep; row 3 a
  // sequence that ends on a delay, an endless chain, an empty sequence, a join stopped at once, a looped sequence.
  {name: 'animations', file: fixture('animations'), screen: PORTRAIT},
  {
    name: 'anim-fade',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '28,23',
    settle: 200,
  },
  // The second fade cancels the first, whose completion runs as not finished; the third starts from the second's.
  {
    name: 'anim-fade-again',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '28,23 28,23',
    settle: 600,
  },
  {
    name: 'anim-spring',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '74,23',
    settle: 150,
  },
  {
    name: 'anim-seq',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '120,23',
    settle: 250,
  },
  {
    name: 'anim-seq-done',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '120,23',
    settle: 700,
  },
  {
    name: 'anim-loop',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '166,23',
    settle: 220,
  },
  {
    name: 'anim-loop-done',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '166,23',
    settle: 600,
  },
  // A fade and a sequence, both stopped mid-way.
  {
    name: 'anim-stop',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '28,23 120,23 212,23',
    settle: 600,
  },
  {
    name: 'anim-join',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '28,59',
    settle: 500,
  },
  {
    name: 'anim-join-done',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '28,59',
    settle: 900,
  },
  // The wait's timing, then the stagger cuts it: its completion runs as not finished, so says nothing.
  {
    name: 'anim-wait',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '74,59',
    settle: 300,
  },
  {
    name: 'anim-wait-done',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '74,59',
    settle: 600,
  },
  {
    name: 'anim-stagger',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '120,59',
    settle: 150,
  },
  {
    name: 'anim-decay',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '166,59',
    settle: 200,
  },
  {
    name: 'anim-sleep',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '212,59',
    settle: 400,
  },
  {
    name: 'anim-held',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '28,95',
    settle: 500,
  },
  // The endless chain, an empty sequence's completion at once, and a join stopped as it starts.
  {
    name: 'anim-ever',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '74,95 120,95 166,95',
    settle: 400,
  },
  {
    name: 'anim-twice',
    file: fixture('animations'),
    screen: PORTRAIT,
    taps: '212,95',
    settle: 420,
  },
  {name: 'interpolate', file: fixture('interpolate'), screen: PORTRAIT},
  {
    name: 'interpolate-spin',
    file: fixture('interpolate'),
    screen: PORTRAIT,
    settle: 250,
  },
  // A press of each <TouchableOpacity>, the frame taken while the second fades back.
  {
    name: 'interpolate-press',
    file: fixture('interpolate'),
    screen: PORTRAIT,
    taps: '80,174 80,218',
    settle: 60,
  },
  {
    name: 'interpolate-pressed',
    file: fixture('interpolate'),
    screen: PORTRAIT,
    taps: '80,174 80,174',
    settle: 500,
  },
  {name: 'drag', file: fixture('drag'), screen: PORTRAIT},
  // A tap on the knob: a grant and a release with no travel.
  {name: 'drag-tap', file: fixture('drag'), screen: PORTRAIT, taps: '50,86'},
  // The knob dragged right, then back from where it landed.
  {
    name: 'drag-knob',
    file: fixture('drag'),
    screen: PORTRAIT,
    taps: '50,86>130,96 130,86>70,80',
  },
  // The pad, claimed once the finger is past its slop, so its travel starts from there.
  {
    name: 'drag-pad',
    file: fixture('drag'),
    screen: PORTRAIT,
    taps: '40,154>200,150',
  },
  // The second row dragged, then the third: each move names its own row.
  {
    name: 'drag-rows',
    file: fixture('drag-rows'),
    screen: PORTRAIT,
    taps: '60,74>120,84 60,120>90,120',
  },
  {name: 'svg', file: fixture('svg'), screen: PORTRAIT},
  // The arc twice, then the toggle (a wider stroke and the gradient) and the mode (a stop's and a stroke's color).
  {
    name: 'svg-state',
    file: fixture('svg'),
    screen: PORTRAIT,
    taps: '50,120 50,120 33,195 91,195',
  },
  // updateVector three times with a hint in ints, then twice with one in floats.
  {
    name: 'svg-update',
    file: fixture('svg'),
    screen: PORTRAIT,
    taps: '149,195 149,195 149,195 207,195 207,195',
  },
  // Two rows added, then the toggle redraws every row. Four rows keep the drawings within the host's vector nodes.
  {
    name: 'svg-rows',
    file: fixture('svg-rows'),
    screen: PORTRAIT,
    listCap: 4,
    taps: '33,23 33,23 91,23',
  },
  // The app's own keyboard, brought up by focusing the input.
  {
    name: 'keyboard',
    file: fixture('keyboard'),
    screen: PORTRAIT,
    taps: '120,30',
  },
  // "ab", the shift layer (its key lit), "C", a backspace, "E" and a space.
  {
    name: 'keyboard-typed',
    file: fixture('keyboard'),
    screen: PORTRAIT,
    taps: '120,30 24,245 72,245 24,275 120,245 216,275 216,245 120,275',
  },
  // Then done, which puts the keyboard away.
  {
    name: 'keyboard-done',
    file: fixture('keyboard'),
    screen: PORTRAIT,
    taps: '120,30 24,245 72,245 120,305',
  },
  {
    name: 'keyboard-look',
    file: fixture('keyboard-look'),
    screen: PORTRAIT,
    taps: '120,30',
  },
  // One package with named panels, at each of them.
  {
    name: 'responsive-panels-portrait',
    file: fixture('responsive'),
    screen: PORTRAIT,
    panels: [
      [240, 320],
      [320, 240],
    ],
  },
  {
    name: 'responsive-panels-landscape',
    file: fixture('responsive'),
    screen: [320, 240],
    panels: [
      [240, 320],
      [320, 240],
    ],
  },
  {name: 'dial', file: fixture('dial'), screen: PORTRAIT},
  // A tap on the first dial's track, then one on each end of the range's.
  {
    name: 'dial-taps',
    file: fixture('dial'),
    screen: PORTRAIT,
    taps: '14,62 226,62',
  },
  {name: 'modal', file: fixture('modal'), screen: PORTRAIT},
  {
    name: 'modal-open',
    file: fixture('modal'),
    screen: PORTRAIT,
    taps: '120,32',
  },
  {
    name: 'modal-closed',
    file: fixture('modal'),
    screen: PORTRAIT,
    taps: '120,32 120,175',
  },
  // A spinner's angle moves with the clock, so two frames apart differ.
  {name: 'activity', file: fixture('activity'), screen: PORTRAIT},
  {
    name: 'activity-later',
    file: fixture('activity'),
    screen: PORTRAIT,
    settle: 180,
  },
  {
    name: 'activity-stopped',
    file: fixture('activity'),
    screen: PORTRAIT,
    taps: '120,80',
  },
  {name: 'scroll', file: fixture('scroll'), screen: PORTRAIT},
  {
    name: 'scroll-dragged',
    file: fixture('scroll'),
    screen: PORTRAIT,
    taps: '120,200>120,80',
    settle: 1000,
  },
  {
    name: 'scroll-pick',
    file: fixture('scroll'),
    screen: PORTRAIT,
    taps: '120,120',
  },
  {name: 'flat-list', file: fixture('flat-list'), screen: PORTRAIT},
  // A row added, then a press on it.
  {
    name: 'flat-list-taps',
    file: fixture('flat-list'),
    screen: PORTRAIT,
    taps: '120,26 120,132',
  },
  {name: 'image', file: fixture('image'), screen: PORTRAIT},
  {
    name: 'image-swap',
    file: fixture('image'),
    screen: PORTRAIT,
    taps: '144,80',
  },
  {name: 'controls', file: fixture('controls'), screen: PORTRAIT},
  {
    name: 'controls-switch',
    file: fixture('controls'),
    screen: PORTRAIT,
    taps: '32,48',
  },
  {
    name: 'controls-focus',
    file: fixture('controls'),
    screen: PORTRAIT,
    taps: '120,84',
  },
  // Focus, then the keyboard's OK.
  {
    name: 'controls-submit',
    file: fixture('controls'),
    screen: PORTRAIT,
    taps: '120,84 215,307',
  },
  {name: 'events', file: fixture('events'), screen: PORTRAIT},
  // A press held still past delayLongPress, then a drag across the touch box.
  {
    name: 'events-long',
    file: fixture('events'),
    screen: PORTRAIT,
    taps: '120,28>120,28 120,120>180,120',
  },
  // Both disabled presses, then the toggle hides the dot.
  {
    name: 'events-disabled',
    file: fixture('events'),
    screen: PORTRAIT,
    taps: '60,74 180,74 120,212',
  },
  {name: 'sizes', file: fixture('sizes'), screen: PORTRAIT},
  {name: 'svg-paint', file: fixture('svg-paint'), screen: PORTRAIT},
  {name: 'svg-source', file: fixture('svg-source'), screen: PORTRAIT},
  {name: 'hooks', file: fixture('hooks'), screen: PORTRAIT},
  {
    name: 'hooks-taps',
    file: fixture('hooks'),
    screen: PORTRAIT,
    taps: '120,82 120,82 120,122',
  },
];

/** Runs a command, throwing with its output when it fails. */
function run(command: string, args: string[]): SpawnSyncReturns<string> {
  const result = spawnSync(command, args, {encoding: 'utf8'});
  if (result.status !== 0) {
    throw new Error(
      `${command} ${args.join(' ')} failed:\n${result.stdout || ''}${result.stderr || ''}`,
    );
  }
  return result;
}

/** Writes an ARGB8888 frame as a PNG, which drops its alpha. */
function writePng(
  path: string,
  pixels: Uint32Array,
  [width, height]: Size,
): void {
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

/** Reads the host's frame. */
const readFrame = (path: string): Uint32Array => {
  const bytes = readFileSync(path);
  return new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.length / 4);
};

/** Reads a golden image as an ARGB8888 frame, or null when it is missing or isn't @p width by @p height. */
function readGolden(path: string, [width, height]: Size): Uint32Array | null {
  if (!existsSync(path)) {
    return null;
  }
  const png = PNG.sync.read(readFileSync(path));
  if (png.width !== width || png.height !== height) {
    return null;
  }
  const frame = new Uint32Array(width * height);
  for (let index = 0; index < frame.length; index++) {
    frame[index] =
      (OPAQUE_BLACK |
        (png.data[index * 4] << 16) |
        (png.data[index * 4 + 1] << 8) |
        png.data[index * 4 + 2]) >>>
      0;
  }
  return frame;
}

/**
 * Compares two frames, ignoring alpha, which a PNG doesn't keep.
 *
 * @returns How many pixels differ by more than GOLDEN_TOLERANCE, and a frame marking them.
 */
function diffFrames(
  left: Uint32Array,
  right: Uint32Array,
): {differing: number; marks: Uint32Array} {
  const channelOff = (
    expected: number,
    actual: number,
    shift: number,
  ): boolean =>
    Math.abs(((expected >>> shift) & 0xff) - ((actual >>> shift) & 0xff)) >
    GOLDEN_TOLERANCE;
  const marks = new Uint32Array(left.length);
  let differing = 0;
  for (let index = 0; index < left.length; index++) {
    if (
      channelOff(left[index], right[index], 16) ||
      channelOff(left[index], right[index], 8) ||
      channelOff(left[index], right[index], 0)
    ) {
      differing++;
      marks[index] = DIFF_MARK;
    }
  }
  return {differing, marks};
}

/**
 * Compiles a scenario to a package.
 *
 * @returns The package's path and its resource report.
 */
async function compileScenario(
  scenario: Scenario,
): Promise<{packagePath: string; report: string[]}> {
  const source = readFileSync(scenario.file, 'utf8');
  const sourceDirectory = dirname(scenario.file);
  const size = ([width, height]: Size): Screen => ({width, height});
  const packaged = compilePackage(source, scenario.name, {
    filename: scenario.file,
    listCap: scenario.listCap,
    svgArtifacts: await bakeSvgArtifacts(source, sourceDirectory),
    screens: scenario.everySize
      ? undefined
      : (scenario.panels ?? [scenario.screen]).map(size),
    screen: scenario.everySize ? undefined : size(scenario.screen),
    release: scenario.release,
  });
  const images = packaged.images.map(image => ({
    name: image.name,
    path: resolve(sourceDirectory, image.importPath),
  }));
  const file = packPackage(
    packaged,
    images.length ? bakeAssetPackBytes({images}) : undefined,
  );
  const packagePath = join(BUILD_DIR, `${scenario.name}.erpkg`);
  writeFileSync(packagePath, file.bytes);
  return {packagePath, report: resourceReport(packaged, file)};
}

/** The built host's path; a multi-config generator puts it under the build type. */
function host(): string {
  const direct = join(BUILD_DIR, `erb-frames${EXE_SUFFIX}`);
  return existsSync(direct)
    ? direct
    : join(BUILD_DIR, 'Release', `erb-frames${EXE_SUFFIX}`);
}

/**
 * Runs the host and compares its frame with the golden image. With `record`, the frame becomes the golden image
 * instead.
 *
 * @returns null when they match, or what differs.
 */
function compareScenario(
  scenario: Scenario,
  packagePath: string,
  record: boolean,
): string | null {
  const [width, height] = scenario.screen;
  const framePath = join(OUT_DIR, `${scenario.name}.raw`);
  run(host(), [
    framePath,
    String(width),
    String(height),
    packagePath,
    scenario.taps || '',
    String(scenario.settle ?? 500),
  ]);

  const frame = readFrame(framePath);
  const goldenPath = join(GOLDEN_DIR, `${scenario.name}.png`);
  if (record) {
    mkdirSync(GOLDEN_DIR, {recursive: true});
    writePng(goldenPath, frame, scenario.screen);
    return null;
  }
  const outputs = join(OUT_DIR, scenario.name);
  const golden = readGolden(goldenPath, scenario.screen);
  if (!golden) {
    writePng(`${outputs}.frame.png`, frame, scenario.screen);
    return `no ${width}x${height} golden image at ${goldenPath}; record it with npm run erb:frames:record ${scenario.name}`;
  }
  const {differing, marks} = diffFrames(golden, frame);
  if (differing) {
    writePng(`${outputs}.golden.png`, golden, scenario.screen);
    writePng(`${outputs}.frame.png`, frame, scenario.screen);
    writePng(`${outputs}.diff.png`, marks, scenario.screen);
    return `${differing} of ${frame.length} pixels differ from the golden image; see ${outputs}.{golden,frame,diff}.png`;
  }
  return null;
}

const args = process.argv.slice(2);
const record = args.includes('--record');
const filter = args.find(argument => !argument.startsWith('--'));
const scenarios = filter
  ? SCENARIOS.filter(scenario => scenario.name.includes(filter))
  : SCENARIOS;
if (!scenarios.length) {
  console.error(
    `No scenarios match "${filter}". Known: ${SCENARIOS.map(scenario => scenario.name).join(', ')}`,
  );
  process.exit(2);
}

rmSync(OUT_DIR, {recursive: true, force: true});
mkdirSync(OUT_DIR, {recursive: true});
run('cmake', ['-S', HOST_DIR, '-B', BUILD_DIR, '-DCMAKE_BUILD_TYPE=Release']);
run('cmake', ['--build', BUILD_DIR, '-j']);

let failed = 0;
for (const scenario of scenarios) {
  const panels =
    scenario.panels && scenario.panels.length > 1
      ? ` of ${scenario.panels.length} panels`
      : '';
  const label = `${scenario.name} @ ${scenario.screen.join('x')}${panels}${scenario.release ? ' release' : ''}${scenario.taps ? ` taps[${scenario.taps}]` : ''}${scenario.settle !== undefined ? ` +${scenario.settle}ms` : ''}`;
  try {
    const {packagePath, report} = await compileScenario(scenario);
    const problem = compareScenario(scenario, packagePath, record);
    if (problem) {
      failed++;
      console.log(`✗ ${label}: ${problem}`);
    } else {
      console.log(`✓ ${label}`);
    }
    console.log(report.join('\n'));
  } catch (error) {
    failed++;
    console.log(
      `✗ ${label}: ${error instanceof Error ? error.message : String(error)}`,
    );
  }
}

console.log(`\n${scenarios.length - failed} matched, ${failed} failed`);
process.exit(failed ? 1 : 0);
