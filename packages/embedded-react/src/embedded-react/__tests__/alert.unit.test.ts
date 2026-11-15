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

// Alert.alert over a mocked NativeUI: what the dialog mounts, what its buttons do, and the queue.
import {describe, it, expect, vi, beforeEach, afterEach} from 'vitest';

interface MockNode {
  type: string;
  props: Record<string, unknown>;
  events: Record<string, (() => void) | null>;
  children: MockNode[];
}

let rootNode: MockNode;

function installMocks() {
  const globals = globalThis as Record<string, unknown>;
  globals.IS_REACT_ACT_ENVIRONMENT = false;
  globals.screen = {width: 240, height: 320};
  globals.NativeUI = {
    createNode: (type: string): MockNode => ({
      type,
      props: {},
      events: {},
      children: [],
    }),
    setProps: (node: MockNode, props: Record<string, unknown>) => {
      Object.assign(node.props, props);
    },
    setRoot: (node: MockNode) => {
      rootNode = node;
    },
    appendChild: (parent: MockNode, child: MockNode) => {
      parent.children.push(child);
    },
    insertBefore: (parent: MockNode, child: MockNode, before: MockNode) => {
      parent.children.splice(parent.children.indexOf(before), 0, child);
    },
    removeChild: (parent: MockNode, child: MockNode) => {
      parent.children.splice(parent.children.indexOf(child), 1);
    },
    destroyNode: () => {},
    setEvent: (node: MockNode, name: string, handler: (() => void) | null) => {
      node.events[name] = handler;
    },
    setTextSpans: () => {},
    setVectorOps: () => {},
    commit: () => {},
    now: () => 0,
  };
}

// React flushes an update made outside a batch in a microtask.
const settle = () => new Promise(resolve => setTimeout(resolve, 0));

/** Every node under `node` of one type, in tree order. */
function findAll(node: MockNode, type: string): MockNode[] {
  const found = node.type === type ? [node] : [];
  return found.concat(...node.children.map(child => findAll(child, type)));
}

const texts = (node: MockNode) =>
  findAll(node, 'Text').map(text => text.props.text);

async function mountApp() {
  const {createElement} = (await import('react')).default;
  const {AppRegistry} = await import('../AppRegistry.js');
  const {Alert} = await import('../Alert.js');
  AppRegistry.registerComponent('demo', () => () => createElement('View', {}));
  return Alert;
}

describe('Alert', () => {
  beforeEach(() => {
    vi.resetModules();
    installMocks();
  });
  afterEach(() => {
    const globals = globalThis as Record<string, unknown>;
    delete globals.NativeUI;
    delete globals.screen;
  });

  it('mounts nothing until an alert is raised', async () => {
    await mountApp();
    expect(rootNode.children.map(child => child.type)).toEqual(['View']);
  });

  it('shows a full-screen modal after the app, with a default OK button', async () => {
    const Alert = await mountApp();
    Alert.alert('Saved', 'All done');
    await settle();

    expect(rootNode.children.map(child => child.type)).toEqual([
      'View',
      'Modal',
    ]);
    const modal = rootNode.children[1];
    expect(modal.props).toMatchObject({visible: true, width: 240, height: 320});
    expect(texts(modal)).toEqual(['Saved', 'All done', 'OK']);
    // 16 px clear of each edge of a 240 px panel.
    expect(modal.children[0].props.width).toBe(208);

    findAll(modal, 'Pressable')[0].events.onPress?.();
    await settle();
    expect(rootNode.children.map(child => child.type)).toEqual(['View']);
  });

  it('closes, then calls the pressed button', async () => {
    const Alert = await mountApp();
    const pressed: string[] = [];
    Alert.alert('Delete?', undefined, [
      {text: 'Cancel', style: 'cancel', onPress: () => pressed.push('cancel')},
      {
        text: 'Delete',
        style: 'destructive',
        onPress: () => pressed.push('delete'),
      },
    ]);
    await settle();

    const modal = rootNode.children[1];
    expect(texts(modal)).toEqual(['Delete?', 'Cancel', 'Delete']);
    const [cancelLabel, deleteLabel] = findAll(modal, 'Text').slice(1);
    expect(deleteLabel.props.color).not.toEqual(cancelLabel.props.color);

    findAll(modal, 'Pressable')[1].events.onPress?.();
    await settle();
    expect(pressed).toEqual(['delete']);
    expect(findAll(rootNode, 'Modal')).toEqual([]);
  });

  it('queues a second alert behind the first', async () => {
    const Alert = await mountApp();
    Alert.alert('First');
    await settle();
    Alert.alert('Second');
    expect(texts(rootNode)).toEqual(['First', 'OK']);

    findAll(rootNode, 'Pressable')[0].events.onPress?.();
    await settle();
    expect(texts(rootNode)).toEqual(['Second', 'OK']);

    findAll(rootNode, 'Pressable')[0].events.onPress?.();
    await settle();
    expect(findAll(rootNode, 'Modal')).toEqual([]);
  });

  it('shows an alert a button handler raises', async () => {
    const Alert = await mountApp();
    Alert.alert('First', undefined, [
      {text: 'Next', onPress: () => Alert.alert('Second')},
    ]);
    await settle();

    findAll(rootNode, 'Pressable')[0].events.onPress?.();
    await settle();
    expect(texts(rootNode)).toEqual(['Second', 'OK']);
  });

  it('shows an alert raised before the app is registered', async () => {
    const {Alert} = await import('../Alert.js');
    Alert.alert('Early');
    await settle();
    await mountApp();
    expect(texts(rootNode)).toEqual(['Early', 'OK']);
  });

  it('closes a cancelable alert on a tap outside, not on a tap on the sheet', async () => {
    const Alert = await mountApp();
    let dismissed = 0;
    Alert.alert('Note', 'Tap outside', undefined, {
      cancelable: true,
      onDismiss: () => dismissed++,
    });
    await settle();

    // Backdrop, sheet, OK.
    const [backdrop, sheet] = findAll(rootNode, 'Pressable');
    sheet.events.onPress?.();
    await settle();
    expect(findAll(rootNode, 'Modal').length).toBe(1);

    backdrop.events.onPress?.();
    await settle();
    expect(dismissed).toBe(1);
    expect(findAll(rootNode, 'Modal')).toEqual([]);
  });

  it('has no backdrop press target unless cancelable', async () => {
    const Alert = await mountApp();
    Alert.alert('Note');
    await settle();
    expect(findAll(rootNode, 'Pressable').length).toBe(1);
  });
});
