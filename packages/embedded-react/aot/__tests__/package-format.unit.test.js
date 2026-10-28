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
 * The package writer keeps its own copy of the format's codes, since the npm package doesn't ship the C
 * headers. This reads the headers and checks every copied value against them.
 */

import {describe, it, expect} from 'vitest';
import {readFileSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import * as format from '../compiler/package/format.mts';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../../..');
const formatHeader = readFileSync(
  resolve(root, 'runtime/erb_format.h'),
  'utf8',
);
// The span props are in a macro: join its continued lines.
const propsSource = readFileSync(
  resolve(root, 'runtime/erb_props.c'),
  'utf8',
).replace(/\\\n/g, ' ');
const propsHeader = readFileSync(resolve(root, 'runtime/erb_props.h'), 'utf8');
const sceneHeader = readFileSync(
  resolve(root, 'engine/include/er_scene.h'),
  'utf8',
);

/** Every `NAME = value` and implicit enumerator in a header's enums, by name. */
function enumValues(source) {
  const values = new Map();
  const withoutComments = source
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/\/\/.*$/gm, '');
  for (const [, body] of withoutComments.matchAll(
    /enum\s*\w*\s*\{([^}]*)\}/g,
  )) {
    let next = 0;
    for (const entry of body.split(',')) {
      const match = entry.trim().match(/^(\w+)(?:\s*=\s*(.+))?$/);
      if (!match) continue;
      const [, name, value] = match;
      const resolved =
        value === undefined
          ? next
          : /^\w+$/.test(value.trim()) && values.has(value.trim())
            ? values.get(value.trim())
            : Number.parseInt(value, 0 || undefined);
      values.set(name, resolved);
      next = resolved + 1;
    }
  }
  return values;
}

/** A `#define NAME <number>` from a header. */
function defineValue(source, name) {
  const match = source.match(
    new RegExp(`#define ${name}\\s+(0x[0-9A-Fa-f]+|\\d+)u?\\b`),
  );
  return match ? Number(match[1]) : undefined;
}

const formatEnums = enumValues(formatHeader);
const sceneEnums = enumValues(sceneHeader);

describe('package format copy', () => {
  it('has the runtime opcode list, in order, with each form and length', () => {
    const opcodes = [
      ...formatHeader.matchAll(/X\((\w+),\s*ERB_F_(ABC|ABX),\s*([12]),/g),
    ].map(([, name, form, words]) => [name, form.toLowerCase(), Number(words)]);
    expect(opcodes.length).toBeGreaterThan(100);
    expect(format.OPCODES).toEqual(opcodes);
  });

  it('has the runtime versions, sizes and codes', () => {
    expect(format.FORMAT_MAJOR).toBe(
      defineValue(formatHeader, 'ERB_FORMAT_MAJOR'),
    );
    expect(format.FORMAT_MINOR).toBe(
      defineValue(formatHeader, 'ERB_FORMAT_MINOR'),
    );
    expect(format.FLAG_DEBUG).toBe(defineValue(formatHeader, 'ERB_FLAG_DEBUG'));
    expect(format.NONE).toBe(defineValue(formatHeader, 'ERB_NONE'));
    expect(format.FRAME_CELLS).toBe(
      defineValue(formatHeader, 'ERB_FRAME_CELLS'),
    );
    expect(formatHeader).toContain(`#define ERB_TAG "${format.PACKAGE_TAG}"`);
    expect(format.SECTION_PROGRAM).toBe(formatEnums.get('ERB_SECTION_PROGRAM'));
    expect(format.SECTION_DEBUG).toBe(formatEnums.get('ERB_SECTION_DEBUG'));

    for (const name of [
      'HEADER',
      'PANEL',
      'NEED',
      'DIRECTORY',
      'REGION',
      'SLOT',
      'SET',
      'PROP',
      'TEMPLATE',
      'NODE',
      'BINDING',
      'FUNCTION',
    ]) {
      expect(format[`${name}_SIZE`], name).toBe(
        defineValue(formatHeader, `ERB_${name}_SIZE`),
      );
    }

    const snake = name =>
      name.replace(/[A-Z]/g, letter => `_${letter}`).toUpperCase();
    const groups = [
      [format.Table, 'ERB_TABLE_'],
      [format.Region, 'ERB_REGION_'],
      [format.Resource, 'ERB_RES_'],
      [format.TemplateKind, 'ERB_TPL_'],
      [format.PropKind, 'ERB_PK_'],
    ];
    for (const [group, prefix] of groups) {
      for (const [name, value] of Object.entries(group)) {
        expect(value, `${prefix}${snake(name)}`).toBe(
          formatEnums.get(`${prefix}${snake(name)}`),
        );
      }
    }
  });

  it('has the ABI prop table', () => {
    // An enum's largest value is an er_scene.h enumerator or a define.
    const largest = name =>
      sceneEnums.get(name) ??
      defineValue(propsHeader, name) ??
      defineValue(sceneHeader, name);
    const props = new Map(
      [
        ...propsSource.matchAll(
          /\[(\d+)\] = \{offsetof\(ERProps, ([\w.[\]]+)\), ERB_PK_(\w+), \w+, (\w+)\}/g,
        ),
      ].map(([, id, field, kind, max]) => [
        field,
        kind === 'ENUM'
          ? {id: Number(id), kind: 'enum', max: largest(max)}
          : {id: Number(id), kind: kind.toLowerCase()},
      ]),
    );
    expect(props.size).toBe(150);
    const spanFields = enumValues(propsHeader);
    const spanKinds = [
      ...propsSource.matchAll(
        /ERB_SPAN_(\w+)\)\]\s*=\s*\{\s*offsetof\(ERProps,\s*spans\[span\]\.(\w+)\),\s*ERB_PK_(\w+)/g,
      ),
    ];
    expect(spanKinds.length).toBe(6);
    for (const [, spanName, field, kind] of spanKinds) {
      for (let span = 0; span < format.MAX_SPANS; span++) {
        props.set(`spans[${span}].${field}`, {
          id:
            defineValue(propsHeader, 'ERB_PROP_SPAN_FIRST') +
            span * defineValue(propsHeader, 'ERB_PROP_SPAN_STRIDE') +
            spanFields.get(`ERB_SPAN_${spanName}`),
          kind: kind.toLowerCase(),
        });
      }
    }
    expect(new Map(format.ABI_PROPS)).toEqual(props);
  });

  it('has the engine ABI level and the er_scene.h values constant props use', () => {
    expect(format.ENGINE_ABI).toBe(defineValue(sceneHeader, 'ER_ABI'));
    for (const [name, value] of format.ENGINE_VALUES) {
      if (name === 'ER_LAYOUT_AUTO') {
        expect(sceneHeader).toContain('#define ER_LAYOUT_AUTO INT16_MIN');
        expect(value).toBe(-32768);
      } else {
        expect(value, name).toBe(sceneEnums.get(name));
      }
    }
  });
});
