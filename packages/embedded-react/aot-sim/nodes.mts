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

// The node tree, as the simulator's debugger answers the debug adapter's `nodes` request: each node's type, its rect
// on the screen, its element's place in the source and the props it has set. It reads the engine's descriptor
// (er_debug.h) as gdb/erb.py does on a board, and answers the same.

import type {
  DebugNode,
  DebugTemplate,
} from '../aot/compiler/package/debug-file.mts';
import {App, DebugError, Target} from './values.mts';

const ER_DEBUG_MAGIC = 0x45445245;
const ER_DEBUG_VERSION = 3;
const POINTER_BYTES = 4;
const POOL_COUNT = 7;
const NO_TAG = 0xffff;
const HANDLE_BYTES = 8;
const NO_LENGTH = -0x8000;
const LONGEST_PROP_TEXT = 60;

/** ERDebug's fields. */
const ENGINE = {
  magic: 0,
  version: 4,
  pools: 8,
  node: 8 + POOL_COUNT * POINTER_BYTES,
};
/** ERDebugPool's fields. */
const POOL = {records: 0, count: 4, stride: 6, usedAt: 8};
/** ERDebugNodeFields' offsets, in its order. */
const NODE_FIELDS = [
  'tag',
  'parent',
  'firstChild',
  'nextSibling',
  'serial',
  'type',
  'typeBytes',
  'rect',
  'scrollX',
  'scrollY',
] as const;

/** How a prop is read: a kind, or an enum's names by value. */
type PropKind =
  | 'i16'
  | 'i16z'
  | 'u8z'
  | 'bool'
  | 'opacity'
  | 'color'
  | 'text'
  | 'f32'
  | 'scale'
  | 'percent'
  | (string | null)[];

/**
 * A node's props, in ERDebugProp's order: its name there, its name in the app, how it is read, and the nodes that
 * have it. An enum's value of 0 or with no name isn't shown.
 */
export const NODE_PROPS: [string, string, PropKind, string][] = [
  ['LAYOUT_LEFT', 'left', 'i16', 'all'],
  ['LAYOUT_TOP', 'top', 'i16', 'all'],
  ['LAYOUT_RIGHT', 'right', 'i16', 'all'],
  ['LAYOUT_BOTTOM', 'bottom', 'i16', 'all'],
  ['LAYOUT_WIDTH', 'width', 'i16', 'all'],
  ['LAYOUT_HEIGHT', 'height', 'i16', 'all'],
  ['LAYOUT_MIN_WIDTH', 'minWidth', 'i16', 'all'],
  ['LAYOUT_MAX_WIDTH', 'maxWidth', 'i16', 'all'],
  ['LAYOUT_MIN_HEIGHT', 'minHeight', 'i16', 'all'],
  ['LAYOUT_MAX_HEIGHT', 'maxHeight', 'i16', 'all'],
  ['LAYOUT_PADDING', 'padding', 'i16', 'all'],
  ['LAYOUT_PADDING_LEFT', 'paddingLeft', 'i16', 'all'],
  ['LAYOUT_PADDING_TOP', 'paddingTop', 'i16', 'all'],
  ['LAYOUT_PADDING_RIGHT', 'paddingRight', 'i16', 'all'],
  ['LAYOUT_PADDING_BOTTOM', 'paddingBottom', 'i16', 'all'],
  ['LAYOUT_MARGIN', 'margin', 'i16', 'all'],
  ['LAYOUT_MARGIN_LEFT', 'marginLeft', 'i16', 'all'],
  ['LAYOUT_MARGIN_TOP', 'marginTop', 'i16', 'all'],
  ['LAYOUT_MARGIN_RIGHT', 'marginRight', 'i16', 'all'],
  ['LAYOUT_MARGIN_BOTTOM', 'marginBottom', 'i16', 'all'],
  ['LAYOUT_GAP', 'gap', 'i16', 'all'],
  ['LAYOUT_ROW_GAP', 'rowGap', 'i16', 'all'],
  ['LAYOUT_COLUMN_GAP', 'columnGap', 'i16', 'all'],
  ['LAYOUT_FLEX_GROW', 'flexGrow', 'i16z', 'all'],
  ['LAYOUT_FLEX_SHRINK', 'flexShrink', 'i16z', 'all'],
  ['LAYOUT_FLEX_BASIS', 'flexBasis', 'i16', 'all'],
  [
    'LAYOUT_FLEX_DIRECTION',
    'flexDirection',
    [null, 'row', 'row-reverse', 'column-reverse'],
    'all',
  ],
  ['LAYOUT_FLEX_WRAP', 'flexWrap', [null, 'wrap', 'wrap-reverse'], 'all'],
  [
    'LAYOUT_ALIGN_ITEMS',
    'alignItems',
    [null, 'stretch', 'flex-start', 'center', 'flex-end'],
    'all',
  ],
  [
    'LAYOUT_ALIGN_SELF',
    'alignSelf',
    [null, 'stretch', 'flex-start', 'center', 'flex-end'],
    'all',
  ],
  [
    'LAYOUT_ALIGN_CONTENT',
    'alignContent',
    [null, 'flex-end', 'center', 'stretch', 'space-between', 'space-around'],
    'all',
  ],
  [
    'LAYOUT_JUSTIFY_CONTENT',
    'justifyContent',
    [
      null,
      'center',
      'flex-end',
      'space-between',
      'space-around',
      'space-evenly',
    ],
    'all',
  ],
  ['LAYOUT_POSITION', 'position', [null, 'absolute'], 'all'],
  ['LAYOUT_DISPLAY', 'display', [null, 'none'], 'all'],
  ['LAYOUT_OVERFLOW', 'overflow', [null, 'hidden', 'scroll'], 'all'],
  ['LAYOUT_ASPECT_RATIO', 'aspectRatio', 'f32', 'all'],
  ['LAYOUT_FLEX_BASIS_PERCENT', 'flexBasis', 'percent', 'all'],
  ['LAYOUT_WIDTH_PERCENT', 'width', 'percent', 'all'],
  ['LAYOUT_HEIGHT_PERCENT', 'height', 'percent', 'all'],
  ['LAYOUT_LEFT_PERCENT', 'left', 'percent', 'all'],
  ['LAYOUT_TOP_PERCENT', 'top', 'percent', 'all'],
  ['LAYOUT_RIGHT_PERCENT', 'right', 'percent', 'all'],
  ['LAYOUT_BOTTOM_PERCENT', 'bottom', 'percent', 'all'],
  ['Z_INDEX', 'zIndex', 'i16z', 'all'],
  ['TRANSLATE_X', 'translateX', 'f32', 'all'],
  ['TRANSLATE_Y', 'translateY', 'f32', 'all'],
  ['SCALE_X', 'scaleX', 'scale', 'all'],
  ['SCALE_Y', 'scaleY', 'scale', 'all'],
  ['ROTATE', 'rotate', 'f32', 'all'],
  ['ROTATE_X', 'rotateX', 'f32', 'all'],
  ['ROTATE_Y', 'rotateY', 'f32', 'all'],
  ['PERSPECTIVE', 'perspective', 'f32', 'all'],
  ['VIEW_BACKGROUND_COLOR', 'backgroundColor', 'color', 'view'],
  [
    'VIEW_BACKGROUND_IMAGE',
    'backgroundImage',
    [null, 'linear-gradient', 'radial-gradient', 'conic-gradient'],
    'view',
  ],
  ['VIEW_BORDER_COLOR', 'borderColor', 'color', 'view'],
  ['VIEW_BORDER_WIDTH', 'borderWidth', 'i16z', 'view'],
  ['VIEW_BORDER_LEFT_WIDTH', 'borderLeftWidth', 'i16z', 'view'],
  ['VIEW_BORDER_TOP_WIDTH', 'borderTopWidth', 'i16z', 'view'],
  ['VIEW_BORDER_RIGHT_WIDTH', 'borderRightWidth', 'i16z', 'view'],
  ['VIEW_BORDER_BOTTOM_WIDTH', 'borderBottomWidth', 'i16z', 'view'],
  ['VIEW_BORDER_STYLE', 'borderStyle', [null, 'dashed', 'dotted'], 'view'],
  ['VIEW_BORDER_RADIUS', 'borderRadius', 'i16z', 'view'],
  ['VIEW_BORDER_TOP_LEFT_RADIUS', 'borderTopLeftRadius', 'i16z', 'view'],
  ['VIEW_BORDER_TOP_RIGHT_RADIUS', 'borderTopRightRadius', 'i16z', 'view'],
  [
    'VIEW_BORDER_BOTTOM_RIGHT_RADIUS',
    'borderBottomRightRadius',
    'i16z',
    'view',
  ],
  ['VIEW_BORDER_BOTTOM_LEFT_RADIUS', 'borderBottomLeftRadius', 'i16z', 'view'],
  ['VIEW_OPACITY', 'opacity', 'opacity', 'view'],
  ['VIEW_SHADOW_OPACITY', 'shadowOpacity', 'f32', 'view'],
  ['VIEW_SHADOW_RADIUS', 'shadowRadius', 'u8z', 'view'],
  ['VIEW_ELEVATION', 'elevation', 'u8z', 'view'],
  ['MODAL_VISIBLE', 'visible', 'bool', 'modal'],
  ['MODAL_BACKDROP_COLOR', 'backdropColor', 'color', 'modal'],
  ['TEXT_TEXT', 'text', 'text', 'text'],
  ['TEXT_COLOR', 'color', 'color', 'text'],
  ['TEXT_FONT_FAMILY', 'fontFamily', 'text', 'text'],
  ['TEXT_FONT_SIZE', 'fontSize', 'u8z', 'text'],
  ['TEXT_FONT_WEIGHT', 'fontWeight', [null, 'bold'], 'text'],
  ['TEXT_FONT_STYLE', 'fontStyle', [null, 'italic'], 'text'],
  ['TEXT_TEXT_ALIGN', 'textAlign', [null, 'center', 'right'], 'text'],
  ['TEXT_NUMBER_OF_LINES', 'numberOfLines', 'u8z', 'text'],
  ['TEXT_LINE_HEIGHT', 'lineHeight', 'i16z', 'text'],
  ['TEXT_LETTER_SPACING', 'letterSpacing', 'i16z', 'text'],
  [
    'TEXT_TEXT_DECORATION',
    'textDecorationLine',
    [null, 'underline', 'line-through'],
    'text',
  ],
  ['TEXT_OPACITY', 'opacity', 'opacity', 'text'],
  ['IMAGE_SOURCE', 'source', 'text', 'image'],
  [
    'IMAGE_RESIZE_MODE',
    'resizeMode',
    [null, 'contain', 'stretch', 'repeat', 'center'],
    'image',
  ],
  ['IMAGE_TINT_COLOR', 'tintColor', 'color', 'image'],
  ['IMAGE_OPACITY', 'opacity', 'opacity', 'image'],
  ['INPUT_VALUE', 'value', 'text', 'text_input'],
  ['INPUT_PLACEHOLDER', 'placeholder', 'text', 'text_input'],
  ['INPUT_COLOR', 'color', 'color', 'text_input'],
  ['INPUT_FONT_FAMILY', 'fontFamily', 'text', 'text_input'],
  ['INPUT_FONT_SIZE', 'fontSize', 'u8z', 'text_input'],
  ['INPUT_BACKGROUND_COLOR', 'backgroundColor', 'color', 'text_input'],
  ['INPUT_BORDER_COLOR', 'borderColor', 'color', 'text_input'],
  ['INPUT_BORDER_WIDTH', 'borderWidth', 'i16z', 'text_input'],
  ['INPUT_BORDER_RADIUS', 'borderRadius', 'i16z', 'text_input'],
  ['INPUT_OPACITY', 'opacity', 'opacity', 'text_input'],
  ['SWITCH_VALUE', 'value', 'bool', 'switch'],
  ['SWITCH_TRACK_COLOR_FALSE', 'trackColor.false', 'color', 'switch'],
  ['SWITCH_TRACK_COLOR_TRUE', 'trackColor.true', 'color', 'switch'],
  ['SWITCH_THUMB_COLOR', 'thumbColor', 'color', 'switch'],
  ['INDICATOR_COLOR', 'color', 'color', 'activity_indicator'],
  ['INDICATOR_ANIMATING', 'animating', 'bool', 'activity_indicator'],
  ['ARC_VALUE', 'value', 'f32', 'arc'],
  ['ARC_MIN', 'min', 'f32', 'arc'],
  ['ARC_MAX', 'max', 'f32', 'arc'],
  ['ARC_START_ANGLE', 'startAngle', 'f32', 'arc'],
  ['ARC_SWEEP_ANGLE', 'sweepAngle', 'f32', 'arc'],
  ['ARC_WIDTH', 'thickness', 'i16z', 'arc'],
  ['ARC_TRACK_COLOR', 'trackColor', 'color', 'arc'],
  ['ARC_INDICATOR_COLOR', 'indicatorColor', 'color', 'arc'],
];

const VIEW_NODES = ['view', 'scroll_view', 'flat_list', 'pressable', 'modal'];

/** A node of the tree, as the adapter gets it. */
export interface TreeNode {
  tag: number;
  type: string;
  /** Left, top, width and height on the screen. */
  rect: [number, number, number, number];
  /** Its element's file, by its index in the debug file's files, with its line and column, or null for none. */
  file: number | null;
  line: number | null;
  column: number | null;
  /** Where the components it is made inside are used, outermost first: each a file, a line and a column. */
  uses: [number, number, number][];
  /** The props it has set: each one's name and value. */
  props: [string, string][];
  children: TreeNode[];
}

const decoder = new TextDecoder('utf-8');

/**
 * A number as JavaScript prints it, to three decimals.
 *
 * @param value  The number.
 *
 * @returns Its text.
 */
const numberText = (value: number): string =>
  String(Math.round(value * 1000) / 1000);

/**
 * A prop as the app's source would say it.
 *
 * @param kind  How it is read.
 * @param record  The node's record.
 * @param at  The prop's offset in it.
 *
 * @returns Its text, or null for one that isn't set.
 */
function propText(kind: PropKind, record: DataView, at: number): string | null {
  if (Array.isArray(kind)) {
    return kind[record.getUint8(at)] ?? null;
  }
  switch (kind) {
    case 'text': {
      const bytes = new Uint8Array(record.buffer, record.byteOffset + at);
      const end = bytes.indexOf(0);
      let text = decoder.decode(bytes.subarray(0, end < 0 ? undefined : end));
      if (text.length > LONGEST_PROP_TEXT) {
        text = `${text.slice(0, LONGEST_PROP_TEXT)}…`;
      }
      return text ? JSON.stringify(text) : null;
    }
    case 'i16':
    case 'i16z': {
      const value = record.getInt16(at, true);
      return value === NO_LENGTH || (kind === 'i16z' && value === 0)
        ? null
        : String(value);
    }
    case 'opacity': {
      const value = record.getUint8(at);
      return value === 255 ? null : String(Math.round(value / 2.55) / 100);
    }
    case 'u8z':
    case 'bool': {
      const value = record.getUint8(at);
      if (value === 0) {
        return null;
      }
      return kind === 'bool' ? 'true' : String(value);
    }
    case 'color': {
      const value = record.getUint32(at, true);
      if (value === 0) {
        return null;
      }
      const alpha = value >>> 24;
      const color = (value & 0xffffff).toString(16).padStart(6, '0');
      return `#${color}${alpha === 255 ? '' : alpha.toString(16).padStart(2, '0')}`;
    }
    default: {
      const value = record.getFloat32(at, true);
      if (
        Number.isNaN(value) ||
        value === 0 ||
        (kind === 'scale' && value === 1)
      ) {
        return null;
      }
      return kind === 'percent' ? `${numberText(value)}%` : numberText(value);
    }
  }
}

/**
 * Each template node's children, by index, from the pre-order list and its child counts.
 *
 * @param template  The template.
 *
 * @returns The children's indexes, for each node.
 */
function templateChildren(template: DebugTemplate): number[][] {
  const children: number[][] = template.nodes.map(() => []);
  const walk = (index: number): number => {
    let following = index + 1;
    for (let child = 0; child < template.nodes[index].children; child++) {
      children[index].push(following);
      following = walk(following);
    }
    return following;
  };
  if (template.nodes.length) {
    walk(0);
  }
  return children;
}

/**
 * An engine node type as the tree names it.
 *
 * @param name  The ER_NODE_* name.
 *
 * @returns The name in lower case, without its prefix.
 */
const shortType = (name: string): string =>
  name.replace('ER_NODE_', '').toLowerCase();

/**
 * Reads the node tree of the running app.
 *
 * @param app  The app.
 * @param engineAddress  Where the engine's descriptor is.
 *
 * @returns The tops: the root alone once the app has committed.
 */
export function nodeTree(app: App, engineAddress: number): TreeNode[] {
  const target: Target = app.target;
  if (target.u32(engineAddress + ENGINE.magic) !== ER_DEBUG_MAGIC) {
    throw new DebugError("the simulator's engine has no debug descriptor");
  }
  const version = target.u16(engineAddress + ENGINE.version);
  if (version !== ER_DEBUG_VERSION) {
    throw new DebugError(
      `the simulator's engine descriptor is version ${version}, but this debugger reads version ${ER_DEBUG_VERSION}: the prebuilt simulator doesn't match this embedded-react`,
    );
  }
  const offsets = (start: number, count: number): number[] =>
    Array.from({length: count}, (_unused, index) =>
      target.u16(engineAddress + start + 2 * index),
    );
  const fieldOffsets = offsets(ENGINE.node, NODE_FIELDS.length);
  const fields = Object.fromEntries(
    NODE_FIELDS.map((name, index) => [name, fieldOffsets[index]]),
  ) as Record<(typeof NODE_FIELDS)[number], number>;
  const propOffsets = offsets(
    ENGINE.node + 2 * NODE_FIELDS.length,
    NODE_PROPS.length,
  );
  const rootTagAddress = target.u32(
    engineAddress +
      Math.ceil(
        (ENGINE.node + 2 * (NODE_FIELDS.length + NODE_PROPS.length)) /
          POINTER_BYTES,
      ) *
        POINTER_BYTES,
  );
  const poolAddress = target.u32(engineAddress + ENGINE.pools);
  const pool = {
    records: target.u32(poolAddress + POOL.records),
    count: target.u16(poolAddress + POOL.count),
    stride: target.u16(poolAddress + POOL.stride),
    usedAt: target.u16(poolAddress + POOL.usedAt),
  };

  // The nodes in use, by tag, read from the root down.
  const nodes = new Map<number, DataView>();
  const recordOf = (tag: number): DataView | null => {
    const bytes = target.read(pool.records + tag * pool.stride, pool.stride);
    return bytes[pool.usedAt]
      ? new DataView(bytes.buffer, bytes.byteOffset, bytes.length)
      : null;
  };
  const root = target.u16(rootTagAddress);
  const waiting = [root];
  while (waiting.length) {
    const tag = waiting.pop()!;
    if (tag >= pool.count || nodes.has(tag)) {
      continue;
    }
    const record = recordOf(tag);
    if (record) {
      nodes.set(tag, record);
      waiting.push(
        record.getUint16(fields.nextSibling, true),
        record.getUint16(fields.firstChild, true),
      );
    }
  }
  if (!nodes.size) {
    // Before the app's first commit the engine has no root yet.
    for (let tag = 0; tag < pool.count; tag++) {
      const record = recordOf(tag);
      if (record) {
        nodes.set(tag, record);
      }
    }
  }

  const typeName = (record: DataView): string => {
    const number =
      fields.typeBytes === 1
        ? record.getUint8(fields.type)
        : fields.typeBytes === 2
          ? record.getUint16(fields.type, true)
          : record.getUint32(fields.type, true);
    return shortType(app.debugFile.nodeTypes[number] || `type ${number}`);
  };
  const engineChildren = (tag: number): number[] => {
    const children: number[] = [];
    let child = nodes.get(tag)!.getUint16(fields.firstChild, true);
    while (child !== NO_TAG && nodes.has(child) && !children.includes(child)) {
      children.push(child);
      child = nodes.get(child)!.getUint16(fields.nextSibling, true);
    }
    return children;
  };

  // Each node code reaches, by its handle.
  const templates = app.panel.templates;
  const places = new Map<number, [number, number]>();
  templates.forEach((template, templateIndex) => {
    template.nodes.forEach((node, nodeIndex) => {
      const handle = node.handle;
      if (!handle) {
        return;
      }
      for (let row = 0; row < handle.count; row++) {
        const at = app.arena + handle.offset + row * HANDLE_BYTES;
        const tag = target.i32(at) - 1;
        const record = nodes.get(tag);
        if (
          record &&
          record.getUint32(fields.serial, true) === target.u32(at + 4)
        ) {
          places.set(tag, [templateIndex, nodeIndex]);
        }
      }
    });
  });
  const childrenOf = templates.map(templateChildren);

  // The rest are matched to their template by where they sit under a node whose place is known.
  const match = (tag: number): void => {
    const place = places.get(tag);
    if (!place) {
      return;
    }
    const [templateIndex, nodeIndex] = place;
    const template = templates[templateIndex];
    let unclaimed = [...childrenOf[templateIndex][nodeIndex]];
    for (const child of engineChildren(tag)) {
      const known = places.get(child);
      if (known) {
        if (known[0] === templateIndex && unclaimed.includes(known[1])) {
          unclaimed = unclaimed.slice(unclaimed.indexOf(known[1]) + 1);
        }
      } else if (
        unclaimed.length &&
        shortType(template.nodes[unclaimed[0]].type) ===
          typeName(nodes.get(child)!)
      ) {
        places.set(child, [templateIndex, unclaimed.shift()!]);
      }
      match(child);
    }
  };

  const tops = nodes.has(root)
    ? [root]
    : [...nodes.keys()].filter(
        tag => !nodes.has(nodes.get(tag)!.getUint16(fields.parent, true)),
      );
  if (tops.length && !places.has(tops[0]) && templates[0]?.nodes.length) {
    places.set(tops[0], [0, 0]);
  }
  tops.forEach(match);

  // Layout places a node on the screen; a ScrollView above it moves it by its offset.
  const built = (
    tag: number,
    scrolledX: number,
    scrolledY: number,
  ): TreeNode => {
    const record = nodes.get(tag)!;
    const place = places.get(tag);
    const element: DebugNode | null = place
      ? templates[place[0]].nodes[place[1]]
      : null;
    const placed = element !== null && element.line !== null;
    const type = typeName(record);
    const groups = [
      'all',
      type,
      ...(VIEW_NODES.includes(type) ? ['view'] : []),
    ];
    // A size in percent is said for the size.
    const props = new Map<string, string>();
    NODE_PROPS.forEach(([, name, kind, group], index) => {
      if (groups.includes(group)) {
        const text = propText(kind, record, propOffsets[index]);
        if (text !== null) {
          props.set(name, text);
        }
      }
    });
    const scrollX =
      scrolledX + Math.trunc(record.getFloat32(fields.scrollX, true));
    const scrollY =
      scrolledY + Math.trunc(record.getFloat32(fields.scrollY, true));
    return {
      tag,
      type,
      rect: [
        record.getInt16(fields.rect, true) - scrolledX,
        record.getInt16(fields.rect + 2, true) - scrolledY,
        record.getInt16(fields.rect + 4, true),
        record.getInt16(fields.rect + 6, true),
      ],
      file: placed ? (element.file ?? 0) : null,
      line: placed ? element.line : null,
      column: placed ? (element.column ?? null) : null,
      uses: placed
        ? (element.uses ?? []).map(use => [use.file ?? 0, use.line, use.column])
        : [],
      props: [...props],
      children: engineChildren(tag).map(child =>
        built(child, scrollX, scrollY),
      ),
    };
  };
  return tops.map(top => built(top, 0, 0));
}
