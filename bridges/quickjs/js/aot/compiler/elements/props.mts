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

import {RN_PLATFORM_NO_OPS} from '../../../src/embedded-react/warn-props.js';
import {isStyleKey} from '../../style-map.mts';
import {aotError} from '../diagnostics.mts';
import type * as t from '@babel/types';
import type {AotError} from '../diagnostics.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** What an element may hold: engine nodes, the text of a <Text>, the shapes of an <Svg>, or nothing. */
export type ChildKind = 'nodes' | 'text' | 'shapes' | 'none';

/** Everything an element accepts. An attribute, style key or child outside it is an AOT error. */
export interface ElementSpec {
  /** The tag, as errors print it. */
  tag: string;
  /** Every attribute the element lowers, `key` aside. */
  props: ReadonlySet<string>;
  /** Attributes React Native or Flow A know that Flow B refuses, each with what to do instead. */
  refused: Readonly<Record<string, string>>;
  /** Whether it takes the RN props that wrap an OS service the board does not have (testID and the like). */
  platformNoOps: boolean;
  /** The style keys its node lays out or draws with, or null when it takes no style at all. */
  styleKeys: ReadonlySet<string> | null;
  /** The style keys the engine animates on its node, when bound to an animated value. */
  animatedStyleKeys: ReadonlySet<string>;
  /** What its children may be. */
  children: ChildKind;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** A JSX event prop → the engine event it registers a handler for. */
export const EVENT_TYPES: Record<string, string> = {
  onPress: 'ER_EVENT_PRESS',
  onLongPress: 'ER_EVENT_LONG_PRESS',
  onPressIn: 'ER_EVENT_PRESS_IN',
  onPressOut: 'ER_EVENT_PRESS_OUT',
  onTouchStart: 'ER_EVENT_TOUCH_START',
  onTouchMove: 'ER_EVENT_TOUCH_MOVE',
  onTouchEnd: 'ER_EVENT_TOUCH_END',
  onLayout: 'ER_EVENT_LAYOUT',
};

/** Style keys every node lays out by, and `transform`, which every node draws through. */
const LAYOUT_STYLE = [
  'width',
  'height',
  'flexBasis',
  'minWidth',
  'maxWidth',
  'minHeight',
  'maxHeight',
  'padding',
  'paddingHorizontal',
  'paddingVertical',
  'paddingLeft',
  'paddingTop',
  'paddingRight',
  'paddingBottom',
  'margin',
  'marginHorizontal',
  'marginVertical',
  'marginLeft',
  'marginTop',
  'marginRight',
  'marginBottom',
  'gap',
  'rowGap',
  'columnGap',
  'flex',
  'flexGrow',
  'flexShrink',
  'flexDirection',
  'alignItems',
  'alignSelf',
  'justifyContent',
  'position',
  'display',
  'left',
  'top',
  'right',
  'bottom',
  'zIndex',
  'transform',
];

/** What the engine draws a View, ScrollView or Pressable with. */
const VIEW_STYLE = [
  'backgroundColor',
  'borderRadius',
  'borderTopLeftRadius',
  'borderTopRightRadius',
  'borderBottomRightRadius',
  'borderBottomLeftRadius',
  'borderWidth',
  'borderColor',
  'opacity',
];

/** What the engine draws a Text with. */
const TEXT_STYLE = [
  'color',
  'fontSize',
  'fontWeight',
  'lineHeight',
  'letterSpacing',
];

/** What the engine draws a Modal's own box with. */
const MODAL_STYLE = [
  'backgroundColor',
  'borderRadius',
  'borderWidth',
  'borderColor',
  'opacity',
];

/** What the engine draws a TextInput with. */
const TEXT_INPUT_STYLE = [...MODAL_STYLE, 'color', 'fontSize'];

/** The style keys the engine animates on a View, ScrollView, Pressable or Modal; `transform` animates anywhere. */
const VIEW_ANIMATED = ['opacity', 'backgroundColor', 'transform'];

/** The style keys the engine animates on a Text. */
const TEXT_ANIMATED = ['color', 'transform'];

/** What a span of a multi-span <Text> carries; the rest of its look comes from the outer <Text>. */
const SPAN_STYLE = ['color', 'fontSize', 'fontWeight', 'letterSpacing'];

/** The paint a vector shape or group takes. */
const SVG_PAINT = [
  'fill',
  'stroke',
  'strokeWidth',
  'strokeLinecap',
  'strokeLinejoin',
  'strokeMiterlimit',
  'fillRule',
  'fillGrad',
  'strokeGrad',
];

/** The attributes every host node lowers: style, visibility, a node ref, and its events. */
const HOST_PROPS = [
  'style',
  'visible',
  'ref',
  'delayLongPress',
  ...Object.keys(EVENT_TYPES),
];

/** Why the engine's TextInput refuses a React Native prop it has no field for. */
const NO_TEXT_INPUT_FIELD =
  "the engine's TextInput has no such setting, so the device would ignore it. Remove it.";

/** Why an <Svg> refuses an event: its node is a drawing, not a touch target of its own. */
const SVG_EVENT =
  'wrap it in a <Pressable> (or a <View>) that takes the event: <Pressable onPress={…}><Svg …/></Pressable>.';

/** Why an <Svg> refuses paint on itself: Flow A does not inherit it either. */
const SVG_ROOT_PAINT =
  'paint goes on the shapes, or on a <G> around them; the <Svg> itself passes none down.';

/** The built-in elements, by tag. `Animated.X` resolves to X. */
const ELEMENTS: Record<string, ElementSpec> = {
  View: element(
    'View',
    HOST_PROPS,
    [...LAYOUT_STYLE, ...VIEW_STYLE],
    'nodes',
    {},
    VIEW_ANIMATED,
  ),
  ScrollView: element(
    'ScrollView',
    HOST_PROPS,
    [...LAYOUT_STYLE, ...VIEW_STYLE],
    'nodes',
    {
      onScroll:
        'scroll events are not lowered yet. Read the position another way, or keep this screen Flow A only.',
      horizontal:
        "the engine's ScrollView scrolls vertically; lay a horizontal strip out with flexDirection: 'row'.",
    },
    VIEW_ANIMATED,
  ),
  Pressable: element(
    'Pressable',
    [...HOST_PROPS, 'disabled'],
    [...LAYOUT_STYLE, ...VIEW_STYLE],
    'nodes',
    {},
    VIEW_ANIMATED,
  ),
  TouchableOpacity: element(
    'TouchableOpacity',
    [...HOST_PROPS, 'disabled', 'activeOpacity'],
    [...LAYOUT_STYLE, ...VIEW_STYLE],
    'nodes',
    {},
    VIEW_ANIMATED,
  ),
  Text: element(
    'Text',
    [...HOST_PROPS, 'numberOfLines', 'ellipsizeMode'],
    [...LAYOUT_STYLE, ...TEXT_STYLE],
    'text',
    {},
    TEXT_ANIMATED,
  ),
  Image: element(
    'Image',
    [...HOST_PROPS, 'source', 'imageName', 'resizeMode', 'tintColor'],
    LAYOUT_STYLE,
    'none',
  ),
  Switch: element(
    'Switch',
    [
      'style',
      'ref',
      'value',
      'onValueChange',
      'trackColor',
      'thumbColor',
      'disabled',
    ],
    LAYOUT_STYLE,
    'none',
  ),
  TextInput: element(
    'TextInput',
    [
      'style',
      'ref',
      'value',
      'defaultValue',
      'onChangeText',
      'onSubmitEditing',
      'onFocus',
      'onBlur',
      'placeholder',
      'placeholderTextColor',
      'cursorColor',
      'editable',
      'secureTextEntry',
    ],
    [...LAYOUT_STYLE, ...TEXT_INPUT_STYLE],
    'none',
    Object.fromEntries(
      [
        'autoFocus',
        'keyboardType',
        'maxLength',
        'multiline',
        'autoCapitalize',
        'autoCorrect',
        'returnKeyType',
      ].map(name => [name, NO_TEXT_INPUT_FIELD]),
    ),
  ),
  ActivityIndicator: element(
    'ActivityIndicator',
    ['style', 'ref', 'color', 'size', 'animating'],
    [...LAYOUT_STYLE, 'color'],
    'none',
  ),
  Modal: element(
    'Modal',
    [
      'style',
      'ref',
      'visible',
      'backdropColor',
      'animationType',
      'onRequestClose',
      'statusBarTranslucent',
    ],
    [...LAYOUT_STYLE, ...MODAL_STYLE],
    'nodes',
    {
      transparent:
        'the engine always draws a backdrop behind a Modal, in backdropColor; set backdropColor="transparent" for none.',
    },
    VIEW_ANIMATED,
  ),
  Dial: element(
    'Dial',
    [
      'style',
      'ref',
      'value',
      'min',
      'max',
      'startAngle',
      'sweepAngle',
      'step',
      'thickness',
      'bandThickness',
      'trackColor',
      'indicatorColor',
      'indicatorGradient',
      'bandColor',
      'cap',
      'segments',
      'gapAngle',
      'knob',
      'knobSize',
      'minSpan',
      'knobColor',
      'knobBorderColor',
      'knobBorderWidth',
      'knobImage',
      'adjustable',
      'range',
      'valueStart',
      'onChange',
    ],
    LAYOUT_STYLE,
    'nodes',
  ),
  FlatList: {
    ...element(
      'FlatList',
      ['data', 'renderItem', 'keyExtractor', 'style'],
      [...LAYOUT_STYLE, ...VIEW_STYLE],
      'none',
      {
        ref: 'Flow A cannot pass a ref through <FlatList> either; use <ScrollView ref={…}> with .map.',
      },
    ),
    // The Flow A wrapper warns about these too, so a list that runs in the simulator also compiles.
    platformNoOps: false,
  },
  Svg: element(
    'Svg',
    ['style', 'ref', 'width', 'height', 'viewBox', 'source'],
    LAYOUT_STYLE,
    'shapes',
    {
      visible:
        'wrap it: <View visible={…}><Svg …/></View> — the View carries the hide and the whole subtree goes with it.',
      ...Object.fromEntries(
        Object.keys(EVENT_TYPES).map(name => [name, SVG_EVENT]),
      ),
      ...Object.fromEntries(SVG_PAINT.map(name => [name, SVG_ROOT_PAINT])),
    },
  ),
};

/** The shapes inside an <Svg>, by tag; they are drawing data, so they take no ref, style or events. */
const SVG_SHAPES: Record<string, ElementSpec> = {
  Path: shape('Path', ['d']),
  Circle: shape('Circle', ['cx', 'cy', 'r']),
  Ellipse: shape('Ellipse', ['cx', 'cy', 'rx', 'ry']),
  Rect: shape('Rect', ['x', 'y', 'width', 'height', 'rx', 'ry']),
  Line: shape('Line', ['x1', 'y1', 'x2', 'y2']),
  Arc: shape('Arc', ['cx', 'cy', 'r', 'startAngle', 'endAngle']),
  G: {
    ...shape('G', ['x', 'y', 'translateX', 'translateY', 'scale']),
    children: 'shapes',
  },
};

/** A <Text> nested in a <Text>: one inline span of the outer one. */
export const TEXT_SPAN: ElementSpec = element(
  'Text',
  ['style'],
  SPAN_STYLE,
  'text',
  {},
  [],
);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Declares an element.
 *
 * @param tag  The tag.
 * @param props  The attributes it lowers.
 * @param styleKeys  The style keys it lays out or draws with.
 * @param children  What it may hold.
 * @param refused  Known attributes it refuses, with what to do instead.
 * @param animatedStyleKeys  The style keys the engine animates on it.
 *
 * @returns The declaration.
 */
function element(
  tag: string,
  props: string[],
  styleKeys: string[],
  children: ChildKind,
  refused: Record<string, string> = {},
  animatedStyleKeys: string[] = ['transform'],
): ElementSpec {
  return {
    tag,
    props: new Set(props),
    refused,
    platformNoOps: true,
    styleKeys: new Set(styleKeys),
    animatedStyleKeys: new Set(animatedStyleKeys),
    children,
  };
}

/**
 * Declares a vector shape: its geometry plus the paint every shape takes.
 *
 * @param tag  The tag.
 * @param geometry  The attributes that place it.
 *
 * @returns The declaration.
 */
function shape(tag: string, geometry: string[]): ElementSpec {
  return {
    tag,
    props: new Set([...geometry, ...SVG_PAINT]),
    refused: {},
    platformNoOps: false,
    styleKeys: null,
    animatedStyleKeys: new Set(),
    children: 'none',
  };
}

/** Every built-in element's tag. */
export const ELEMENT_TAGS = Object.keys(ELEMENTS);

/** Every shape's tag. */
export const SVG_SHAPE_TAGS = Object.keys(SVG_SHAPES);

/**
 * The declaration of a built-in element.
 *
 * @param tag  The resolved tag.
 *
 * @returns The declaration, or undefined when `tag` is not a built-in element.
 */
export const elementSpec = (tag: string): ElementSpec | undefined =>
  Object.hasOwn(ELEMENTS, tag) ? ELEMENTS[tag] : undefined;

/**
 * The declaration of a shape inside an <Svg>.
 *
 * @param tag  The tag.
 *
 * @returns The declaration, or undefined when `tag` is not a shape.
 */
export const svgShapeSpec = (tag: string): ElementSpec | undefined =>
  Object.hasOwn(SVG_SHAPES, tag) ? SVG_SHAPES[tag] : undefined;

/**
 * An attribute's name as written, a namespaced one (`xlink:href`) included.
 *
 * @param attr  The attribute.
 *
 * @returns Its name.
 */
export const attrName = (attr: t.JSXAttribute): string =>
  attr.name.type === 'JSXNamespacedName'
    ? `${attr.name.namespace.name}:${attr.name.name.name}`
    : attr.name.name;

/**
 * Pins an error to a construct's location.
 *
 * @param error  The error.
 * @param node  The construct.
 *
 * @returns The error, for the caller to throw.
 */
export function at(error: AotError, node: t.Node): AotError {
  if (node.loc) {
    error.aotLoc = node.loc.start;
  }

  return error;
}

/**
 * Whether an element takes an attribute: `key`, a declared prop, or a platform no-op it accepts.
 *
 * @param spec  The element.
 * @param name  The attribute's name.
 *
 * @returns Whether it is declared.
 */
const declares = (spec: ElementSpec, name: string): boolean =>
  name === 'key' ||
  spec.props.has(name) ||
  (spec.platformNoOps && RN_PLATFORM_NO_OPS.includes(name));

/**
 * Whether `attr` sits on a built-in element or shape that does not declare it. A component's props are its own,
 * so an attribute on one is never undeclared.
 *
 * @param openingElement  The element's opening tag.
 * @param attr  One of its attributes.
 *
 * @returns Whether the element would refuse it.
 */
export function isUndeclaredAttribute(
  openingElement: t.JSXOpeningElement,
  attr: t.JSXAttribute,
): boolean {
  const nameNode = openingElement.name;
  const tag =
    nameNode.type === 'JSXIdentifier'
      ? nameNode.name
      : nameNode.type === 'JSXMemberExpression' &&
          nameNode.object.type === 'JSXIdentifier' &&
          nameNode.object.name === 'Animated'
        ? nameNode.property.name
        : null;
  const spec =
    tag == null ? undefined : (elementSpec(tag) ?? svgShapeSpec(tag));
  return spec != null && !declares(spec, attrName(attr));
}

/**
 * Refuses every attribute `spec` does not declare, at the attribute. Spreads are left to the element's
 * emitter, which knows the one spread it may take.
 *
 * @param spec  The element.
 * @param openingElement  Its opening tag.
 */
export function checkAttributes(
  spec: ElementSpec,
  openingElement: t.JSXOpeningElement,
): void {
  for (const attr of openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') continue;

    const name = attrName(attr);
    if (declares(spec, name)) continue;

    const refusal = Object.hasOwn(spec.refused, name)
      ? spec.refused[name]
      : undefined;
    throw at(
      aotError(
        `AOT: <${spec.tag}> prop "${name}" is not supported`,
        refusal ?? `supported props: ${[...spec.props].join(', ')}.`,
      ),
      attr,
    );
  }
}

/**
 * `element` without the platform no-op props its declaration accepts, so no emitter has to know them. A copy
 * is returned only when one is dropped.
 *
 * @param spec  The element.
 * @param element  The element, its attributes already checked.
 *
 * @returns The element, or a copy of it without those attributes.
 */
export function withoutPlatformNoOps(
  spec: ElementSpec,
  element: t.JSXElement,
): t.JSXElement {
  if (!spec.platformNoOps) return element;

  const attrs = element.openingElement.attributes;
  const kept = attrs.filter(
    attr =>
      attr.type !== 'JSXAttribute' ||
      !RN_PLATFORM_NO_OPS.includes(attrName(attr)),
  );
  return kept.length === attrs.length
    ? element
    : {
        ...element,
        openingElement: {...element.openingElement, attributes: kept},
      };
}

/**
 * Refuses a style key the element's node neither lays out nor draws with. A key no element lowers is left to
 * the style lowering, which says so in its own words.
 *
 * @param spec  The element.
 * @param key  The style key.
 * @param node  Where the key is written, for the error's location.
 */
export function checkStyleKey(
  spec: ElementSpec,
  key: string,
  node: t.Node,
): void {
  if (key !== 'transform' && !isStyleKey(key)) return;
  if (spec.styleKeys?.has(key)) return;

  const drawn = [...(spec.styleKeys ?? [])].filter(
    styleKey => !LAYOUT_STYLE.includes(styleKey),
  );
  throw at(
    aotError(
      `AOT: style "${key}" has no effect on <${spec.tag}>`,
      `the engine lays a <${spec.tag}> out like any node` +
        (drawn.length ? ` and draws it with ${drawn.join(', ')}` : '') +
        ', so the device would ignore it. Move it to a wrapping <View>, or remove it.',
    ),
    node,
  );
}

/**
 * Refuses a style key bound to an animated value where the engine does not animate it on this element's node.
 *
 * @param spec  The element.
 * @param key  The style key.
 * @param node  Where the key is written, for the error's location.
 */
export function checkAnimatedStyleKey(
  spec: ElementSpec,
  key: string,
  node: t.Node,
): void {
  checkStyleKey(spec, key, node);
  if (spec.animatedStyleKeys.has(key)) return;

  throw at(
    aotError(
      `AOT: an animated "${key}" has no effect on <${spec.tag}>`,
      'the engine animates opacity and backgroundColor on a View, ScrollView, Pressable or Modal, color on ' +
        'a Text, and transform anywhere. Animate a wrapping <View>, or drive the value from state.',
    ),
    node,
  );
}

/**
 * Refuses children on an element that holds none. Whitespace between tags and JSX comments are not children.
 *
 * @param spec  The element.
 * @param children  Its children.
 */
export function checkNoChildren(
  spec: ElementSpec,
  children: t.JSXElement['children'],
): void {
  for (const child of children) {
    if (child.type === 'JSXText' && !child.value.trim()) continue;
    if (
      child.type === 'JSXExpressionContainer' &&
      child.expression.type === 'JSXEmptyExpression'
    ) {
      continue;
    }

    throw at(
      aotError(
        `AOT: <${spec.tag}> takes no children`,
        `the engine draws a <${spec.tag}> on its own; put what belongs beside it in a wrapping <View>.`,
      ),
      child,
    );
  }
}

/**
 * `children` with each `<>…</>` replaced by its own children, at any depth: a fragment adds no node of its own.
 *
 * @param children  An element's children.
 *
 * @returns The children, fragments inlined.
 */
export function inlineFragments(
  children: t.JSXElement['children'],
): Exclude<t.JSXElement['children'][number], t.JSXFragment>[] {
  return children.flatMap(child =>
    child.type === 'JSXFragment' ? inlineFragments(child.children) : [child],
  );
}
