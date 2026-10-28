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
 * The package backend: lowers the IR to a Flow B program for one panel. The tree becomes the root template
 * and its constant prop sets. What needs bytecode (state, handlers, timers, effects, animations, responders,
 * drawings, the keyboard, values worked out at boot) isn't lowered yet, and is refused with what it is.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError, constantText} from '../diagnostics.mts';
import {hasRuntimeProps} from '../ir/nodes.mts';
import {
  ENGINE_VALUES,
  FLAG_DEBUG,
  FRAME_CELLS,
  NONE,
  Region,
  Resource,
  TemplateKind,
} from './format.mts';
import {constantPropEntry, stringPropEntry} from './props.mts';
import type {IrApp} from '../ir/app.mts';
import type {IrElement, IrNode} from '../ir/nodes.mts';
import type {Screen} from '../options.mts';
import type {
  Program,
  ProgramPropEntry,
  ProgramTemplateNode,
} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The tables one program shares across its pieces, each entry stored once.
 *
 * @class ProgramTables
 */
class ProgramTables {
  readonly strings: string[] = [];
  readonly propSets: ProgramPropEntry[][] = [];
  private readonly stringIndex = new Map<string, number>();
  private readonly propSetIndex = new Map<string, number>();

  /**
   * A string's index, adding it the first time.
   *
   * @param text  The string.
   *
   * @returns Its index.
   */
  string(text: string): number {
    let index = this.stringIndex.get(text);
    if (index === undefined) {
      index = this.strings.push(text) - 1;
      this.stringIndex.set(text, index);
    }

    return index;
  }

  /**
   * A prop set's index, adding it the first time.
   *
   * @param entries  The set's entries, applied in order.
   *
   * @returns Its index, or NONE for an empty set.
   */
  propSet(entries: ProgramPropEntry[]): number {
    if (!entries.length) {
      return NONE;
    }

    const key = JSON.stringify(entries);
    let index = this.propSetIndex.get(key);
    if (index === undefined) {
      index = this.propSets.push(entries) - 1;
      this.propSetIndex.set(key, index);
    }

    return index;
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Refuses what the package output can't lower yet.
 *
 * @param what  What the app uses.
 */
function notYet(what: string): never {
  throw aotError(
    `AOT: the package output doesn't support ${what} yet`,
    'build without --package to get the C output, which supports it.',
  );
}

/**
 * Refuses an app that needs anything besides its tree: those parts are all bytecode.
 *
 * @param app  The app.
 */
function checkStatic(app: IrApp): void {
  const {out} = app;
  const uses: [boolean, string][] = [
    [out.handlers.length > 0, 'event handlers'],
    [out.timerFns.length > 0, 'timers'],
    [out.effectFns.length > 0 || out.mountEffects.length > 0, 'effects'],
    [app.animations.length > 0 || out.animCbs.length > 0, 'animations'],
    [out.panResponders.length > 0, 'PanResponder'],
    [out.vectors.length > 0, '<Svg>'],
    [app.keyboard !== null, 'setKeyboardConfig'],
    [app.boot.slots.length > 0, 'values worked out from the screen size'],
    [app.hasUpdate, 'state that changes what is shown'],
  ];
  for (const [used, what] of uses) {
    if (used) {
      notYet(what);
    }
  }
}

/**
 * A node's constant props as a prop set, in the order the C output sets them.
 *
 * @param node  The node.
 * @param tables  The program's tables.
 *
 * @returns The set's entries.
 */
function nodeProps(node: IrNode, tables: ProgramTables): ProgramPropEntry[] {
  if (hasRuntimeProps(node)) {
    notYet('props, text or images that state drives');
  }

  if (node.spans) {
    notYet('nested <Text> spans');
  }

  const entries = node.constantProps.map(prop =>
    constantPropEntry(prop.field, prop.expr),
  );
  if (node.placeholder != null) {
    entries.push(
      stringPropEntry('placeholder', tables.string(node.placeholder)),
    );
  }

  const text = node.text ? constantText(node.text) : null;
  if (text !== null) {
    entries.push(stringPropEntry('text', tables.string(text)));
  }

  if (node.image?.kind === 'static') {
    entries.push(stringPropEntry('image_name', tables.string(node.image.name)));
  }

  return entries;
}

/**
 * An element and everything under it as template nodes, each before its children.
 *
 * @param element  The element.
 * @param tables  The program's tables.
 * @param nodes  Collects the template nodes.
 */
function templateNodes(
  element: IrElement,
  tables: ProgramTables,
  nodes: ProgramTemplateNode[],
): void {
  if (element.kind === 'component') {
    templateNodes(element.root, tables, nodes);
    return;
  }

  const {node} = element;
  if (node.wiring.some(wire => wire.kind !== 'ref')) {
    notYet(`${node.wiring.find(wire => wire.kind !== 'ref')!.kind} wiring`);
  }

  const children = node.children.map(child =>
    child.kind === 'element'
      ? child.element
      : notYet(
          child.kind === 'pool' ? 'lists of state' : 'conditionals on state',
        ),
  );
  nodes.push({
    type: ENGINE_VALUES.get(node.type)!,
    children: children.length,
    props: tables.propSet(nodeProps(node, tables)),
    handle: NONE,
    bindings: 0,
    bindingCount: 0,
  });
  for (const child of children) {
    templateNodes(child, tables, nodes);
  }
}

/**
 * Lowers an app to a program for one panel.
 *
 * @param app  The lowered app.
 * @param screen  The panel it was lowered for.
 *
 * @returns The program.
 */
export function lowerPackage(app: IrApp, screen: Screen): Program {
  checkStatic(app);
  const tables = new ProgramTables();

  // A panel-sized root the app's tree fills, as er_app_build() makes one.
  const rootProps = [
    constantPropEntry('width', String(screen.width)),
    constantPropEntry('height', String(screen.height)),
  ];
  const nodes: ProgramTemplateNode[] = [
    {
      type: ENGINE_VALUES.get('ER_NODE_VIEW')!,
      children: 1,
      props: tables.propSet(rootProps),
      handle: NONE,
      bindings: 0,
      bindingCount: 0,
    },
  ];
  templateNodes(app.root, tables, nodes);

  // With no functions, the register stack only has to exist.
  const stackCells = FRAME_CELLS;
  const regions = [
    {kind: Region.stack, count: stackCells, offset: 0, bytes: stackCells * 4},
  ];
  const arenaBytes = regions.reduce((sum, region) => sum + region.bytes, 0);

  return {
    flags: FLAG_DEBUG,
    panels: [
      {
        width: screen.width,
        height: screen.height,
        arenaBytes,
        nodeCount: nodes.length,
        rootTemplate: 0,
        bootFunction: NONE,
        updateFunction: NONE,
      },
    ],
    needs: app.images.length
      ? [{resource: Resource.images, amount: app.images.length}]
      : [],
    strings: tables.strings,
    const32: [],
    const64: [],
    regions,
    slots: [],
    propSets: tables.propSets,
    templates: [{kind: TemplateKind.root, nodes}],
    bindings: [],
    functions: [],
  };
}
