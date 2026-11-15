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

import {describe, expect, it} from 'vitest';
import {compilePackage} from '../compile.mts';
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';
import {nodesOf} from '../compiler/ir/nodes.mts';
import type {IrNode} from '../compiler/ir/nodes.mts';

// Alert.alert: the dialog each call builds under the root, and what the compiler refuses. What the dialogs do
// when pressed is erb:frames' job (the alert fixture).

const SCREEN = {screen: {width: 240, height: 320}};

const HEAD =
  "import {useEffect, useState} from 'react';\n" +
  "import {Alert, Modal, Pressable, Text, View} from 'embedded-react';\n";

/** An app whose one button runs `handler`, with `body` before the App's return and `before` at module level. */
const appOf = (handler: string, body = '', before = '') =>
  `${HEAD}${before}\n` +
  'export function App() {\n' +
  '  const [count, setCount] = useState(0);\n' +
  `${body}\n` +
  '  return (\n' +
  '    <View>\n' +
  `      <Pressable onPress={() => { ${handler} }}><Text>{count}</Text></Pressable>\n` +
  '    </View>\n' +
  '  );\n' +
  '}\n';

const lower = (source: string) => lowerApp(source, resolveOptions(SCREEN));

const compile = (source: string) => compilePackage(source, 'App.jsx', SCREEN);

/** The message a compile fails with. */
const failure = (source: string) => {
  try {
    compile(source);
  } catch (error) {
    return (error as Error).message;
  }
  throw new Error('it compiled');
};

/** The nodes of the dialogs: everything after the app's own View, Pressable and Text. */
const dialogOf = (source: string) => nodesOf(lower(source).root).slice(3);

const typesOf = (nodes: IrNode[]) =>
  nodes.map(node => node.type.replace('ER_NODE_', ''));

/** Every node's constant text, or '?' where it is worked out at run time. */
const textsOf = (nodes: IrNode[]) =>
  nodes
    .filter(node => node.text)
    .map(node =>
      node
        .text!.parts.map(part => (part.kind === 'literal' ? part.text : '?'))
        .join(''),
    );

const propsOf = (node: IrNode) =>
  Object.fromEntries(
    node.constantProps.map(assign => [assign.field, assign.value]),
  );

const eventsOf = (node: IrNode) =>
  node.wiring.flatMap(wire => (wire.kind === 'event' ? [wire.event] : []));

describe('Alert.alert', () => {
  it('builds the dialog Alert.js draws', () => {
    const dialog = dialogOf(
      appOf(
        "Alert.alert('Delete?', 'It is gone for good.', [" +
          "{text: 'Cancel', style: 'cancel'}, " +
          "{text: 'Delete', style: 'destructive', onPress: () => setCount(0)}]);",
      ),
    );
    expect(typesOf(dialog)).toEqual([
      'MODAL',
      'VIEW',
      'TEXT',
      'TEXT',
      'VIEW',
      'PRESSABLE',
      'TEXT',
      'PRESSABLE',
      'TEXT',
    ]);
    expect(textsOf(dialog)).toEqual([
      'Delete?',
      'It is gone for good.',
      'Cancel',
      'Delete',
    ]);
    const [, sheet, , , , cancel, cancelLabel, remove, removeLabel] = dialog;
    expect(propsOf(sheet)).toMatchObject({border_radius: 4, gap: 8});
    expect(eventsOf(cancel)).toEqual(['ER_EVENT_PRESS']);
    expect(eventsOf(remove)).toEqual(['ER_EVENT_PRESS']);
    expect(propsOf(removeLabel).color).not.toEqual(propsOf(cancelLabel).color);
  });

  it('compiles to what the dialog written by hand does', () => {
    const alert = appOf(
      "Alert.alert('Reset?', 'It is at ' + count + '.', [{text: 'Reset', onPress: () => setCount(0)}]);",
    );
    const byHand =
      `${HEAD}\n` +
      'export function App() {\n' +
      '  const [count, setCount] = useState(0);\n' +
      '  const [active, setActive] = useState(0);\n' +
      '  return (\n' +
      '    <View>\n' +
      '      <Pressable onPress={() => { setActive(1); }}><Text>{count}</Text></Pressable>\n' +
      '      {active === 1 && (\n' +
      '        <Modal visible>\n' +
      "          <View style={{backgroundColor: '#ffffff', borderRadius: 4, padding: 16, gap: 8, width: Math.min(280, screen.width - 32)}}>\n" +
      "            <Text style={{color: '#000000', fontWeight: 'bold'}}>{'Reset?'}</Text>\n" +
      "            <Text style={{color: '#444444'}}>{'It is at ' + count + '.'}</Text>\n" +
      "            <View style={{flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 8}}>\n" +
      '              <Pressable onPress={() => { setActive(0); setCount(0); }}>\n' +
      "                <Text style={{color: '#2196F3', fontWeight: '500', padding: 8}}>{'Reset'}</Text>\n" +
      '              </Pressable>\n' +
      '            </View>\n' +
      '          </View>\n' +
      '        </Modal>\n' +
      '      )}\n' +
      '    </View>\n' +
      '  );\n' +
      '}\n';
    expect(compile(alert).program).toEqual(compile(byHand).program);
  });

  it('gives an alert with no buttons a single OK, and leaves out a message it has none of', () => {
    for (const call of [
      "Alert.alert('Saved');",
      "Alert.alert('Saved', undefined, []);",
      "Alert.alert('Saved', null, undefined, {});",
    ]) {
      const dialog = dialogOf(appOf(call));
      expect(typesOf(dialog)).toEqual([
        'MODAL',
        'VIEW',
        'TEXT',
        'VIEW',
        'PRESSABLE',
        'TEXT',
      ]);
      expect(textsOf(dialog)).toEqual(['Saved', 'OK']);
    }
  });

  it('makes a cancelable alert a press target around a sheet that is one too', () => {
    const dialog = dialogOf(
      appOf(
        "Alert.alert('Note', 'Tap outside.', undefined, {cancelable: true, onDismiss: () => setCount(1)});",
      ),
    );
    expect(typesOf(dialog).slice(0, 3)).toEqual([
      'MODAL',
      'PRESSABLE',
      'PRESSABLE',
    ]);
    expect(eventsOf(dialog[1])).toEqual(['ER_EVENT_PRESS']);
    expect(eventsOf(dialog[2])).toEqual(['ER_EVENT_PRESS']);
  });

  it('counts an app as building its largest dialog, not all of them', () => {
    const one = compile(
      appOf("Alert.alert('A', 'a', [{text: 'No'}, {text: 'Yes'}]);"),
    );
    const three = compile(
      appOf(
        "if (count === 0) { Alert.alert('A'); } else if (count === 1) { Alert.alert('B', 'b'); } " +
          "else { Alert.alert('C', 'c', [{text: 'No'}, {text: 'Yes'}]); }",
      ),
    );
    expect(three.panels[0].nodeCount).toBe(one.panels[0].nodeCount);
  });

  it('is raised from an effect, a timer, a helper and a child component', () => {
    const sources = [
      appOf(
        'setCount(1);',
        "  useEffect(() => { Alert.alert('Welcome'); }, []);",
      ),
      appOf("setTimeout(() => Alert.alert('Later'), 500);"),
      appOf(
        'warn();',
        "  const warn = () => { Alert.alert('Careful', 'Count is ' + count); };",
      ),
      `${HEAD}\n` +
        'function Ask({name}) {\n' +
        '  const [asked, setAsked] = useState(0);\n' +
        '  return (\n' +
        "    <Pressable onPress={() => Alert.alert('Sure?', name, [{text: 'Yes', onPress: () => setAsked(asked + 1)}])}>\n" +
        '      <Text>{asked}</Text>\n' +
        '    </Pressable>\n' +
        '  );\n' +
        '}\n' +
        'export function App() {\n' +
        '  return <View><Ask name="one" /><Ask name="two" /></View>;\n' +
        '}\n',
    ];
    for (const source of sources) {
      expect(typesOf(nodesOf(lower(source).root))).toContain('MODAL');
    }
  });

  it('follows the name Alert is imported under', () => {
    const source = appOf("Popup.alert('Saved');").replace(
      '{Alert,',
      '{Alert as Popup,',
    );
    expect(textsOf(dialogOf(source))).toEqual(['Saved', 'OK']);
  });

  it('leaves a binding named Alert alone', () => {
    const source = appOf(
      "Alert.alert('Saved');",
      '  const Alert = {alert: 1};',
    );
    expect(failure(source)).toContain(
      'a handler statement must be a state setter',
    );
  });

  it('refuses a dialog that reads what the handler works out', () => {
    expect(
      failure(appOf("const next = count + 1; Alert.alert('Now ' + next);")),
    ).toContain('an alert can\'t read "next"');
    expect(
      failure(
        appOf(
          "const next = count + 1; Alert.alert('Set?', undefined, [{text: 'OK', onPress: () => setCount(next)}]);",
        ),
      ),
    ).toContain('an alert can\'t read "next"');
  });

  it('refuses buttons and options it cannot see', () => {
    expect(
      failure(
        appOf("Alert.alert('A', 'a', buttons);", '', 'const buttons = [];'),
      ),
    ).toContain("an alert's buttons must be written out as an array");
    expect(failure(appOf("Alert.alert('A', 'a', [ok]);"))).toContain(
      "an alert's button must be written out as an object",
    );
    expect(
      failure(
        appOf("Alert.alert('A', 'a', [{text: 'OK', style: 'primary'}]);"),
      ),
    ).toContain("style is 'default', 'cancel' or 'destructive'");
    expect(
      failure(
        appOf("Alert.alert('A', 'a', undefined, {cancelable: count > 0});"),
      ),
    ).toContain("an alert's cancelable must be a compile-time constant");
    expect(
      failure(
        appOf(
          "Alert.alert('A', 'a', [{text: 'OK', onPress: value => setCount(value)}]);",
        ),
      ),
    ).toContain("an alert's handler takes no arguments");
    expect(failure(appOf('Alert.alert();'))).toContain(
      'Alert.alert needs a title',
    );
  });

  it('refuses any other use of Alert', () => {
    expect(failure(appOf("Alert.prompt('Name?');"))).toContain(
      'Alert has only Alert.alert',
    );
    expect(failure(appOf('const show = Alert.alert;'))).toContain(
      'Alert has only Alert.alert',
    );
  });

  it('refuses a call in render', () => {
    expect(failure(appOf('setCount(1);', "  Alert.alert('Hi');"))).toContain(
      "Alert.alert can't be called while rendering",
    );
  });

  it('refuses a root that is not a View', () => {
    const source =
      `${HEAD}\n` +
      'export function App() {\n' +
      "  return <Pressable onPress={() => Alert.alert('Hi')}><Text>go</Text></Pressable>;\n" +
      '}\n';
    expect(failure(source)).toContain('needs a <View> as its root');
  });
});
