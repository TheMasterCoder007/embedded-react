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
import {printText} from './text.mts';
import type {IrNode, IrPropValue, IrShow} from '../ir/nodes.mts';
import type {NodeUpdate} from '../out.mts';
import type {PanResponderRecord} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

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
  return show.negate ? `!(${test})` : test;
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
    case 'foreign':
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
  }
}

/**
 * Wires a PanResponder's already-emitted handlers and queries onto a node.
 *
 * @param pan  The responder.
 * @param nodeId  The node's variable.
 *
 * @returns The C lines.
 */
export function printPanWiring(
  pan: PanResponderRecord,
  nodeId: string,
): string[] {
  return [
    ...pan.emitted!.events.map(
      ([event, handler]) =>
        `    er_event_set(${nodeId}, ${event}, ${handler}, NULL);`,
    ),
    ...pan.emitted!.queries.map(
      ([query, fn]) =>
        `    er_responder_query_set(${nodeId}, ${query}, ${fn}, NULL);`,
    ),
  ];
}

/**
 * A host node as C. A node with no state-driven content sets its props once; otherwise it keeps a handle and
 * app_update() sets them all again. Spans, animated binds, refs, events and PanResponders are wired once.
 *
 * @param node  The node.
 *
 * @returns Its build lines and app_update() entry.
 */
export function printNode(node: IrNode): PrintedNode {
  const {id} = node;
  const text = node.text ? printText(node.text) : null;
  const imageCode = node.image?.kind === 'foreign' ? node.image.code : null;
  const isDynamic =
    (text?.args.length ?? 0) > 0 ||
    node.stateProps.length > 0 ||
    imageCode !== null;

  const build = [`    ${id} = er_node_create(${node.type});`];
  let update: NodeUpdate | null = null;
  if (isDynamic) {
    build.push(`    s_${id} = ${id};`);
    update = {
      nodeId: id,
      styleAssigns: node.constantProps,
      text,
      dynAssigns: node.stateProps,
      imageName: imageCode,
    };
  } else {
    build.push(`    er_props_default(&p);`);
    for (const prop of node.constantProps) {
      build.push(`    p.${prop.field} = ${prop.expr};`);
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

  // Animated values move their props on the native driver; `interp` remaps the value first.
  node.binds.forEach((bind, bindIndex) => {
    if (bind.interp) {
      const interp = bind.interp;
      build.push(
        `    {`,
        `        static const ERInterpolation interp_${id}_${bindIndex} = { { ` +
          `${interp.input.map(floatLit).join(', ')} }, { ` +
          `${interp.output.map(floatLit).join(', ')} }, ${interp.input.length}, ` +
          `${interp.exLeft}, ${interp.exRight} };`,
        `        er_anim_value_bind_interpolated(${bind.cVar}, ${id}, ${bind.prop}, ` +
          `&interp_${id}_${bindIndex});`,
        `    }`,
      );
    } else {
      build.push(`    er_anim_value_bind(${bind.cVar}, ${id}, ${bind.prop});`);
    }
  });

  for (const ref of node.refs) {
    build.push(`    ${ref} = ${id};`);
  }
  for (const event of node.events) {
    build.push(
      `    er_event_set(${id}, ${event.event}, ${event.handler}, NULL);`,
    );
  }
  for (const pan of node.responders) {
    build.push(...printPanWiring(pan, id));
  }

  return {build, update};
}
