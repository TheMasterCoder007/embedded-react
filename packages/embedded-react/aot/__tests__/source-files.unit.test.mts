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

import {mkdirSync, mkdtempSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {describe, expect, it} from 'vitest';
import {compilePackage, placeOf, readApp, readDebugFile} from '../compile.mts';

const FIXTURES = join(
  dirname(fileURLToPath(import.meta.url)),
  '../../test/erb-frames/fixtures',
);

const OPTIONS = {filename: 'App.tsx', screen: {width: 240, height: 320}};

const HEAD =
  "import React, {useState} from 'react';\n" +
  "import {Pressable, StyleSheet, Text, View} from 'embedded-react';\n";

/** A card that counts its presses, in a file of its own. */
const CARD =
  HEAD +
  'const STEP = 2;\n' +
  'export function Card() {\n' +
  '  const [presses, setPresses] = useState(0);\n' +
  '  return (\n' +
  '    <Pressable onPress={() => setPresses(presses + STEP)}>\n' +
  '      <Text>{presses}</Text>\n' +
  '    </Pressable>\n' +
  '  );\n' +
  '}\n';

/** An app that shows what it imports from `./Card`. */
const appOf = (imports: string, body: string): string =>
  `${HEAD}${imports}\nexport function App() {\n  return <View>${body}</View>;\n}\n`;

const compile = (source: string, sourceFiles: Record<string, string>) =>
  compilePackage(source, 'app', {...OPTIONS, sourceFiles});

/** The program a split app compiles to, as bytes to compare. */
const programOf = (source: string, sourceFiles: Record<string, string> = {}) =>
  Buffer.from(compile(source, sourceFiles).program).toString('hex');

/** The message a compile fails with. */
const failure = (source: string, sourceFiles: Record<string, string>) => {
  try {
    compile(source, sourceFiles);
  } catch (error) {
    return (error as Error).message;
  }
  throw new Error('the compile passed');
};

describe('an app split across files', () => {
  it('compiles to the same program as the app in one file', async () => {
    const split = await readApp(join(FIXTURES, 'split-files.tsx'));
    const whole = await readApp(join(FIXTURES, 'split-files-one.tsx'));
    expect(Object.keys(split.options.sourceFiles).sort()).toEqual([
      'split/Badge.tsx',
      'split/Card.tsx',
      'split/index.ts',
      'split/theme.ts',
      'split/useCounter.ts',
    ]);
    expect(split.paths).toHaveLength(6);
    const screen = OPTIONS.screen;
    const splitResult = compilePackage(split.source, 'app', {
      ...split.options,
      screen,
    });
    const wholeResult = compilePackage(whole.source, 'app', {
      ...whole.options,
      screen,
    });
    expect(Buffer.from(splitResult.program)).toEqual(
      Buffer.from(wholeResult.program),
    );
  });

  it('keeps a name two files use apart', () => {
    const entry =
      HEAD +
      "import {Card} from './Card';\n" +
      'const STEP = 5;\n' +
      'export function App() {\n' +
      '  const [total, setTotal] = useState(0);\n' +
      '  return (\n' +
      '    <View>\n' +
      '      <Card />\n' +
      '      <Pressable onPress={() => setTotal(total + STEP)}>\n' +
      '        <Text>{total}</Text>\n' +
      '      </Pressable>\n' +
      '    </View>\n' +
      '  );\n' +
      '}\n';
    const whole = entry
      .replace("import {Card} from './Card';\n", CARD.slice(HEAD.length))
      .replace('export function Card', 'function Card')
      .replace(/STEP(?= = 2|\)\}>\n {6}<Text>\{presses)/g, 'CARD_STEP');
    expect(whole).toContain('CARD_STEP = 2');
    expect(whole).toContain('presses + CARD_STEP');
    expect(programOf(entry, {'Card.tsx': CARD})).toBe(programOf(whole));
  });

  it('reads an import under another name, and a default export', () => {
    const named = programOf(appOf("import {Card} from './Card';", '<Card />'), {
      'Card.tsx': CARD,
    });
    expect(
      programOf(appOf("import {Card as Tile} from './Card';", '<Tile />'), {
        'Card.tsx': CARD,
      }),
    ).toBe(named);
    expect(
      programOf(appOf("import Tile from './Card';", '<Tile />'), {
        'Card.tsx': CARD.replace('export function', 'export default function'),
      }),
    ).toBe(named);
    expect(
      programOf(appOf("import Tile from './Card';", '<Tile />'), {
        'Card.tsx': CARD.replace(
          'export function Card() {',
          'export default () => {',
        ).replace(/\}\n$/, '};\n'),
      }),
    ).toBe(named);
  });

  it('follows a file that passes another on, and a folder by its index', () => {
    const named = programOf(appOf("import {Card} from './Card';", '<Card />'), {
      'Card.tsx': CARD,
    });
    expect(
      programOf(appOf("import {Card} from './parts';", '<Card />'), {
        'parts/index.ts': "export * from './Card';\n",
        'parts/Card.tsx': CARD,
      }),
    ).toBe(named);
    expect(
      programOf(appOf("import {Tile} from './parts';", '<Tile />'), {
        'parts/index.ts': "export {Card as Tile} from './Card';\n",
        'parts/Card.tsx': CARD,
      }),
    ).toBe(named);
  });

  it('names the file and line of an error in an imported file', () => {
    const message = failure(
      appOf("import {Card} from './parts/Card';", '<Card />'),
      {'parts/Card.tsx': CARD.replace('<Text>{presses}</Text>', '<Blob />')},
    );
    expect(message).toContain('unknown element <Blob>');
    expect(message).toContain('at parts/Card.tsx:8:7');
    expect(message).toContain('>  8 |       <Blob />');
  });

  it('names an imported file with a syntax error', () => {
    expect(
      failure(appOf("import {Card} from './Card';", '<Card />'), {
        'Card.tsx': 'export function Card( {\n',
      }),
    ).toMatch(/^Card\.tsx: .*\(2:0\)/);
  });

  it("gives each line its file in the debug file and the package's debug section", () => {
    const result = compile(
      appOf("import {Card} from './parts/Card';", '<Card />'),
      {'parts/Card.tsx': CARD},
    );
    const debugFile = readDebugFile(result.debugFile);
    expect(debugFile.files).toEqual(['App.tsx', 'parts/Card.tsx']);
    const [panel] = debugFile.panels;
    const handler = panel.functions.findIndex(programFunction =>
      programFunction.lines.some(line => line.file === 1),
    );
    expect(handler).toBeGreaterThanOrEqual(0);
    expect(placeOf(debugFile, handler, 0)[0]).toMatchObject({
      file: 'parts/Card.tsx',
      line: 7,
    });
    // The card's nodes are in its file, and its state belongs to the element in the entry.
    const nodes = panel.templates.flatMap(template => template.nodes);
    expect(nodes.some(node => node.file === 1 && node.line === 7)).toBe(true);
    expect(panel.states[0].component).toEqual({name: 'Card', line: 5});
    expect(Buffer.from(result.debug).toString('latin1')).toContain(
      'parts/Card.tsx',
    );
  });

  it('refuses files that import each other', () => {
    expect(
      failure(appOf("import {Card} from './Card';", '<Card />'), {
        'Card.tsx': "import {STEP} from './step';\n" + CARD,
        'step.ts': "import {Card} from './Card';\nexport const STEP = 2;\n",
      }),
    ).toMatch(/import each other in a loop: Card\.tsx → step\.ts → Card\.tsx/);
    expect(
      failure(appOf("import {Card} from './Card';", '<Card />'), {
        'Card.tsx': "import {App} from './App';\n" + CARD,
      }),
    ).toMatch(/Card\.tsx imports the app's entry, App\.tsx/);
  });

  it('refuses a name a file does not export, and a whole file under one name', () => {
    expect(
      failure(appOf("import {Tile} from './Card';", '<Tile />'), {
        'Card.tsx': CARD,
      }),
    ).toMatch(/Card\.tsx doesn't export Tile\n {2}at App\.tsx:3:9/);
    expect(
      failure(appOf("import Tile from './Card';", '<Tile />'), {
        'Card.tsx': CARD,
      }),
    ).toMatch(/Card\.tsx has no default export/);
    expect(
      failure(appOf("import * as parts from './Card';", '<parts.Card />'), {
        'Card.tsx': CARD,
      }),
    ).toMatch(/`import \* as parts` of one of the app's files isn't supported/);
  });

  it('reads a type an import names without `type`', () => {
    const typed = CARD.replace(
      'const STEP = 2;\n',
      "import {Step} from './types';\nconst STEP: Step = 2;\n",
    );
    expect(
      programOf(appOf("import {Card} from './Card';", '<Card />'), {
        'Card.tsx': typed,
        'types.ts': 'export type Step = number;\n',
      }),
    ).toBe(
      programOf(appOf("import {Card} from './Card';", '<Card />'), {
        'Card.tsx': CARD,
      }),
    );
  });

  it("names an imported file's images and fonts from the entry's folder", () => {
    const card =
      HEAD.replace('Pressable,', 'Image, Pressable,') +
      "import icon from './assets/icon.png';\n" +
      "import logo from '../shared/logo.png';\n" +
      "import Mono from './fonts/Mono.ttf';\n" +
      'export function Card() {\n' +
      '  return (\n' +
      '    <View>\n' +
      '      <Image source={icon} />\n' +
      '      <Image source={logo} />\n' +
      '      <Text style={{fontFamily: Mono, fontSize: 16}}>hi</Text>\n' +
      '    </View>\n' +
      '  );\n' +
      '}\n';
    const result = compile(
      appOf("import {Card} from './parts/Card';", '<Card />'),
      {'parts/Card.tsx': card},
    );
    expect(result.images).toEqual([
      {name: 'icon', importPath: './parts/assets/icon.png'},
      {name: 'logo', importPath: './shared/logo.png'},
    ]);
    expect(result.fonts.map(font => font.importPath)).toEqual([
      './parts/fonts/Mono.ttf',
    ]);
  });

  it('refuses a name two passed-on files export, and takes one both pass on from the same file', () => {
    const other = CARD.replace('const STEP = 2;', 'const STEP = 4;');
    expect(
      failure(appOf("import {Card} from './parts';", '<Card />'), {
        'parts/index.ts': "export * from './Card';\nexport * from './Other';\n",
        'parts/Card.tsx': CARD,
        'parts/Other.tsx': other,
      }),
    ).toMatch(
      /parts\/index\.ts passes Card on from both parts\/Card\.tsx and parts\/Other\.tsx\n {2}at App\.tsx:3:9/,
    );
    expect(
      programOf(appOf("import {Card} from './parts';", '<Card />'), {
        'parts/index.ts': "export * from './a';\nexport * from './b';\n",
        'parts/a.ts': "export * from './Card';\n",
        'parts/b.ts': "export {Card} from './Card';\n",
        'parts/Card.tsx': CARD,
      }),
    ).toBe(
      programOf(appOf("import {Card} from './Card';", '<Card />'), {
        'Card.tsx': CARD,
      }),
    );
  });

  it('refuses two .svg files of one name', () => {
    const svgHead = HEAD.replace('Pressable,', 'Pressable, Svg,');
    expect(
      failure(
        svgHead +
          "import icon from './icon.svg';\n" +
          "import {Card} from './parts/Card';\n" +
          'export function App() {\n' +
          '  return <View><Svg source={icon} /><Card /></View>;\n' +
          '}\n',
        {
          'parts/Card.tsx':
            svgHead +
            "import icon from './icon.svg';\n" +
            'export function Card() {\n' +
            '  return <Svg source={icon} />;\n' +
            '}\n',
        },
      ),
    ).toMatch(
      /two \.svg files are named "icon" \(\.\/parts\/icon\.svg and \.\/icon\.svg\)/,
    );
  });

  it('never merges a file that declares native modules, however it is reached', async () => {
    const directory = mkdtempSync(join(tmpdir(), 'source-files-'));
    const files: Record<string, string> = {
      'App.tsx':
        "import {Pressable, Text} from 'embedded-react';\n" +
        "import {Can} from './device';\n" +
        'export function App() {\n' +
        '  return <Pressable onPress={() => { Can.send(1); }}><Text>send</Text></Pressable>;\n' +
        '}\n',
      'device/index.ts': "export {Can} from './can';\n",
      'device/can.ts':
        "import {nativeModule} from 'embedded-react';\n" +
        'export interface Can { send(id: number): boolean; }\n' +
        "export const Can = nativeModule<Can>('can', 1);\n",
    };
    const write = () => {
      for (const [path, source] of Object.entries(files)) {
        mkdirSync(dirname(join(directory, path)), {recursive: true});
        writeFileSync(join(directory, path), source);
      }
    };
    const failureOf = async () => {
      const app = await readApp(join(directory, 'App.tsx'));
      try {
        compilePackage(app.source, 'app', {...app.options, ...OPTIONS});
      } catch (error) {
        return (error as Error).message;
      }
      throw new Error('the compile passed');
    };

    write();
    expect(await failureOf()).toMatch(
      /device\/index\.ts passes Can on from \.\/can, which declares native modules, and a native module isn't followed further\n {2}at .*device\/index\.ts:1:9/,
    );
    files['device/index.ts'] = "export * from './can';\n";
    write();
    expect(await failureOf()).toMatch(
      /device\/index\.ts passes everything on from \.\/can, which declares native modules/,
    );
    // Imported from the file that declares it, the module is read from there as before.
    files['App.tsx'] = files['App.tsx'].replace('./device', './device/can');
    write();
    const app = await readApp(join(directory, 'App.tsx'));
    expect(Object.keys(app.options.sourceFiles)).toEqual([]);
    expect(
      compilePackage(app.source, 'app', {...app.options, ...OPTIONS}).program
        .length,
    ).toBeGreaterThan(0);
  });
});
