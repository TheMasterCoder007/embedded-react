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

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {cstr, floatLit} from '../c-syntax.mts';
import {printCondition, printExpr} from './expressions.mts';
import {panEventName, panQueryName} from './pan-responder.mts';
import {printText} from './text.mts';
import {printStateDrivenVectorWiring, printVectorOps} from './vectors.mts';
import {branchElements, hasRuntimeProps, isStateDriven} from '../ir/nodes.mts';
import type {IrElement, IrNode, IrPropValue, IrShow} from '../ir/nodes.mts';
import type {IrPanResponder} from '../ir/pan-responder.mts';
import type {AnimBind} from '../style-text.mts';
import type {DynAssign, FormatResult, StaticAssign} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A node whose props depend on state: app_update() rebuilds its ERProps and re-applies them. */
export interface NodeUpdate {
  /** The node's variable (`n<id>`); its handle is the file-scope `s_n<id>`. */
  nodeId: string;
  styleAssigns: StaticAssign[];
  dynAssigns: DynAssign[];
  /** A dynamic text body, as one printf format and its arguments. */
  text: FormatResult | null;
  /** A C expression naming the image to show when the source is dynamic. */
  imageName: string | null;
  /** A static TextInput placeholder. */
  placeholder: string | null;
}

/** A node as C: the lines er_app_build() runs, and the app_update() entry of a node state drives. */
export interface PrintedNode {
  build: string[];
  update: NodeUpdate | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The C condition a conditional's node shows under.
 *
 * @param show  The condition.
 *
 * @returns The C.
 */
export function printShow(show: IrShow): string {
  if (show.kind === 'rowInUse') {
    return `(${show.row} < ${show.list.countMember})`;
  }

  const test = printCondition(show.test);
  const shown = show.negate ? `!(${test})` : test;
  return show.within ? `((${printShow(show.within)}) && (${shown}))` : shown;
}

/**
 * A state-driven prop value as the C written to its ERProps field.
 *
 * @param value  The value.
 *
 * @returns The C.
 */
export function printPropValue(value: IrPropValue): string {
  switch (value.kind) {
    case 'constant':
      return value.code;
    case 'select':
      return (
        `((${printCondition(value.test)}) ? ${printPropValue(value.whenTrue)} : ` +
        `${printPropValue(value.whenFalse)})`
      );
    case 'opacity':
      return `app_opacity(${printExpr(value.value)})`;
    case 'dimension':
      return `app_round_dim(${printExpr(value.value)})`;
    case 'visible':
      return `((${printExpr(value.value)}) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE)`;
    case 'shown':
      return `((${printShow(value.show)}) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE)`;
    case 'whileShown':
      return `((${printShow(value.show)}) ? ${printPropValue(value.value)} : 0)`;
    case 'flag': {
      const test =
        value.truthiness === 'condition'
          ? printCondition(value.test)
          : printExpr(value.test);
      return `(uint8_t)((${value.negate ? `!(${test})` : test}) ? ${value.whenTrue} : 0)`;
    }
    case 'byte':
      return `(uint8_t)(${printPropValue(value.value)})`;
    case 'float':
      return `(float)(${printExpr(value.value)})`;
  }
}

/**
 * An animated value's bind onto a node; an interpolated one first declares its table. The Dial lays its table
 * out one field per line.
 *
 * @param node  The node.
 * @param bind  The bind.
 * @param index  Numbers the table within the node.
 *
 * @returns The C lines.
 */
function printBind(node: IrNode, bind: AnimBind, index: number): string[] {
  if (!bind.interp) {
    return [`    er_anim_value_bind(${bind.cVar}, ${node.id}, ${bind.prop});`];
  }

  const interp = bind.interp;
  const name = `interp_${node.id}_${index}`;
  const input = interp.input.map(floatLit).join(', ');
  const output = interp.output.map(floatLit).join(', ');
  const table =
    node.type === 'ER_NODE_ARC'
      ? [
          `        static const ERInterpolation ${name} = {`,
          `            { ${input} },`,
          `            { ${output} },`,
          `            ${interp.input.length},`,
          `            ${interp.exLeft},`,
          `            ${interp.exRight}`,
          `        };`,
        ]
      : [
          `        static const ERInterpolation ${name} = { { ${input} }, { ${output} }, ` +
            `${interp.input.length}, ${interp.exLeft}, ${interp.exRight} };`,
        ];
  return [
    `    {`,
    ...table,
    `        er_anim_value_bind_interpolated(${bind.cVar}, ${node.id}, ${bind.prop}, &${name});`,
    `    }`,
  ];
}

/**
 * Wires a PanResponder's handlers and queries onto a node.
 *
 * @param pan  The responder.
 * @param nodeId  The node's variable.
 *
 * @returns The C lines.
 */
export function printPanWiring(pan: IrPanResponder, nodeId: string): string[] {
  return [
    ...pan.events.map(
      event =>
        `    er_event_set(${nodeId}, ${event.event}, ${panEventName(pan, event)}, NULL);`,
    ),
    ...pan.queries.map(
      query =>
        `    er_responder_query_set(${nodeId}, ${query.query}, ${panQueryName(pan, query)}, NULL);`,
    ),
  ];
}

/**
 * A node as C. A node with no state-driven content sets its props once; otherwise it keeps a handle and
 * app_update() sets them all again. Spans and the node's wiring are set up once.
 *
 * @param node  The node.
 *
 * @returns Its build lines and app_update() entry.
 */
export function printNode(node: IrNode): PrintedNode {
  const {id} = node;
  const text = node.text ? printText(node.text) : null;
  const imageName =
    node.image?.kind === 'value'
      ? printExpr(node.image.value)
      : node.image
        ? cstr(node.image.name)
        : null;
  const build = [`    ${id} = er_node_create(${node.type});`];
  let update: NodeUpdate | null = null;
  if (isStateDriven(node)) {
    build.push(`    s_${id} = ${id};`);
    update = {
      nodeId: id,
      styleAssigns: node.constantProps,
      text,
      dynAssigns: node.stateProps,
      imageName,
      placeholder: node.placeholder,
    };
  } else if (hasRuntimeProps(node)) {
    // Props that read-only values worked out at boot are set once, here.
    build.push(
      ...printNodeUpdate(
        {
          nodeId: id,
          styleAssigns: node.constantProps,
          text,
          dynAssigns: node.stateProps,
          imageName,
          placeholder: node.placeholder,
        },
        id,
      ),
    );
  } else {
    build.push(`    er_props_default(&p);`);
    for (const prop of node.constantProps) {
      build.push(`    p.${prop.field} = ${prop.expr};`);
    }

    if (node.placeholder != null) {
      build.push(
        `    snprintf(p.placeholder, sizeof(p.placeholder), "%s", ${cstr(node.placeholder)});`,
      );
    }

    if (text) {
      build.push(
        `    snprintf(p.text, sizeof(p.text), "%s", ${cstr(text.format.replace(/%%/g, '%'))});`,
      );
    }

    if (node.image?.kind === 'static') {
      build.push(
        `    snprintf(p.image_name, sizeof(p.image_name), "%s", ${cstr(node.image.name)});`,
      );
    }

    build.push(`    er_node_set_props(${id}, &p);`);
  }

  // Inline text spans are set once; each inherits the node's base style unless it overrides it.
  if (node.spans) {
    build.push(`    {`, `        static const ERTextSpan spans_${id}[] = {`);
    for (const span of node.spans) {
      build.push(
        `            { ${span.text}, ${span.color}, ${span.font_size}, ${span.font_weight}, ` +
          `${span.font_style}, ${span.text_decoration}, ${span.letter_spacing} },`,
      );
    }
    build.push(
      `        };`,
      `        er_node_set_text_spans(${id}, spans_${id}, ${node.spans.length});`,
      `    }`,
    );
  }

  // Binds, refs, events and PanResponders, in the order the element wires them.
  for (const wire of node.wiring) {
    switch (wire.kind) {
      case 'bind':
        build.push(...printBind(node, wire.bind, wire.index));
        break;
      case 'ref':
        build.push(`    ${wire.slot} = ${id};`);
        break;
      case 'event':
        build.push(
          `    er_event_set(${id}, ${wire.event}, ${wire.handler}, NULL);`,
        );
        break;
      case 'responder':
        build.push(...printPanWiring(wire.pan, id));
        break;
      case 'vectorTables':
        build.push(...printVectorOps(id, wire.tables));
        break;
      case 'stateDrivenVector':
        build.push(...printStateDrivenVectorWiring(id, wire.vector));
        break;
    }
  }

  return {build, update};
}

/**
 * Appends a built element to its container.
 *
 * @param parentId  The container's variable.
 * @param child  The element.
 *
 * @returns The C line.
 */
export const printAppend = (parentId: string, child: IrElement): string =>
  `    er_tree_append_child(${parentId}, ${child.id});`;

/**
 * An element and everything under it as er_app_build() lines: each node, then each child's subtree followed by
 * the line that appends it.
 *
 * @param element  The element.
 *
 * @returns The C lines.
 */
export function printElementTree(element: IrElement): string[] {
  if (element.kind === 'component') {
    return printElementTree(element.root);
  }

  const lines = printNode(element.node).build;
  const appendChild = (child: IrElement | null): void => {
    if (child) {
      lines.push(...printElementTree(child), printAppend(element.id, child));
    }
  };
  for (const child of element.node.children) {
    if (child.kind === 'element') {
      appendChild(child.element);
    } else if (child.kind === 'conditional') {
      branchElements(child).forEach(appendChild);
    } else {
      child.rows.forEach(appendChild);
    }
  }

  return lines;
}

/**
 * A state-driven node's app_update() entry: its props built again, then set.
 *
 * @param update  The node's entry.
 * @param target  The node's variable: its file-scope handle, unless er_app_build() sets it.
 *
 * @returns The C lines.
 */
export function printNodeUpdate(
  update: NodeUpdate,
  target = `s_${update.nodeId}`,
): string[] {
  const lines = [`    er_props_default(&p);`];
  for (const assign of update.styleAssigns) {
    lines.push(`    p.${assign.field} = ${assign.expr};`);
  }
  for (const assign of update.dynAssigns) {
    lines.push(`    p.${assign.field} = ${printPropValue(assign.value)};`);
  }
  if (update.placeholder != null) {
    lines.push(
      `    snprintf(p.placeholder, sizeof(p.placeholder), "%s", ${cstr(update.placeholder)});`,
    );
  }
  if (update.imageName != null) {
    lines.push(
      `    snprintf(p.image_name, sizeof(p.image_name), "%s", ${update.imageName});`,
    );
  }
  if (update.text) {
    if (update.text.args.length) {
      lines.push(
        `    snprintf(p.text, sizeof(p.text), ${cstr(update.text.format)}, ${update.text.args.join(', ')});`,
      );
    } else {
      lines.push(
        `    snprintf(p.text, sizeof(p.text), "%s", ${cstr(update.text.format.replace(/%%/g, '%'))});`,
      );
    }
  }
  lines.push(`    er_node_set_props(${target}, &p);`);
  return lines;
}
