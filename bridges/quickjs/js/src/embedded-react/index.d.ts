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

// Public type declarations for the `embedded-react` package — the React Native analog. The runtime is
// JavaScript and ignores types entirely; these only power editors and `tsc`. React hooks (useState,
// useEffect, …) still come from 'react', as in React Native.
//
// What is declared here is what the RUNTIME honors. The sources of truth are the bridge's prop and event
// tables (`k_prop_names` / `event_type_from_name` in bridges/quickjs/native_ui_bridge.c), the top-level
// prop list (`PASSTHROUGH` in src/props.js), and the SVG compiler (src/embedded-react/svg-ops.js). A prop
// the engine ignores is deliberately absent, so a name that typechecks is a name that reaches the engine.
// `npm test` runs a parity test over those tables to keep it that way.
//
// Flow B (the AOT compiler) lowers a documented SUBSET of this surface and fails the build on anything it
// cannot compile, so these types describe Flow A and the AOT reports the difference.

import type {ReactNode, Ref} from 'react';

/** A node handle: what a `ref` on a host component receives, and what the imperative API takes. */
export type NodeHandle = number;

// --- Styling ---------------------------------------------------------------

/**
 * A box dimension. Percentage strings are resolved for `width` and `height` only — every other box
 * property (margins, padding, min/max, borders, insets) takes pixels and ignores a string.
 */
export type DimensionValue = number | `${number}%`;

/** Drop-shadow displacement in px (RN's `shadowOffset`). */
export interface ShadowOffset {
  width: number;
  height: number;
}

/** Fractional pivot `[x, y]` in 0–1 that `transform` rotates and scales about. Defaults to the centre. */
export type TransformOrigin = [number, number];

/**
 * One transform entry. Every axis except `perspective` can be driven by an `AnimatedValue`; `scale` binds
 * both axes at once. Rotations are CSS angle strings ('45deg', '0.5rad').
 */
export type TransformStyle =
  | {scale: number | AnimatedValue}
  | {scaleX: number | AnimatedValue}
  | {scaleY: number | AnimatedValue}
  | {translateX: number | AnimatedValue}
  | {translateY: number | AnimatedValue}
  | {rotate: string | AnimatedValue}
  | {rotateX: string | AnimatedValue}
  | {rotateY: string | AnimatedValue}
  | {rotateZ: string | AnimatedValue}
  | {perspective: number};

/**
 * Value type of an untyped style entry. Styles stay open (a not-yet-declared key is not an error), and the
 * union covers the object- and array-valued entries — `shadowOffset`, `transformOrigin`, `transform` — so
 * they are legal both under their own declarations and through the index signature.
 */
export type StyleValue =
  | string
  | number
  | AnimatedValue
  | TransformStyle[]
  | ShadowOffset
  | TransformOrigin
  | undefined;

/** A single style object. Properties mirror the React Native subset the engine supports. */
export interface ViewStyle {
  [key: string]: StyleValue;

  // Box size.
  width?: DimensionValue;
  height?: DimensionValue;
  minWidth?: number;
  minHeight?: number;
  maxWidth?: number;
  maxHeight?: number;
  aspectRatio?: number;

  // Insets (with `position: 'absolute'`).
  top?: number;
  left?: number;
  right?: number;
  bottom?: number;

  // Margin.
  margin?: number;
  marginTop?: number;
  marginRight?: number;
  marginBottom?: number;
  marginLeft?: number;
  marginHorizontal?: number;
  marginVertical?: number;

  // Padding.
  padding?: number;
  paddingTop?: number;
  paddingRight?: number;
  paddingBottom?: number;
  paddingLeft?: number;
  paddingHorizontal?: number;
  paddingVertical?: number;

  // Flex.
  flex?: number;
  flexGrow?: number;
  flexShrink?: number;
  flexBasis?: number;
  flexDirection?: 'row' | 'column' | 'row-reverse' | 'column-reverse';
  flexWrap?: 'nowrap' | 'wrap' | 'wrap-reverse';
  justifyContent?:
    | 'flex-start'
    | 'flex-end'
    | 'center'
    | 'space-between'
    | 'space-around'
    | 'space-evenly';
  alignItems?: 'auto' | 'flex-start' | 'flex-end' | 'center' | 'stretch';
  alignSelf?: 'auto' | 'flex-start' | 'flex-end' | 'center' | 'stretch';
  alignContent?:
    | 'flex-start'
    | 'flex-end'
    | 'center'
    | 'stretch'
    | 'space-between'
    | 'space-around';
  gap?: number;
  rowGap?: number;
  columnGap?: number;

  // Placement and visibility.
  position?: 'relative' | 'absolute';
  display?: 'flex' | 'none';
  overflow?: 'visible' | 'hidden' | 'scroll';
  zIndex?: number;
  /**
   * Hit-testing behaviour for this node and its subtree. 'none' ignores touches entirely, 'box-none' lets
   * children be touched but not the node itself, 'box-only' the reverse. RN also allows this as a
   * top-level prop; here it is a style entry, which is what the engine reads.
   */
  pointerEvents?: 'auto' | 'none' | 'box-none' | 'box-only';

  // Paint.
  backgroundColor?: string | AnimatedValue;
  opacity?: number | AnimatedValue;

  // Border.
  borderRadius?: number;
  borderTopLeftRadius?: number;
  borderTopRightRadius?: number;
  borderBottomLeftRadius?: number;
  borderBottomRightRadius?: number;
  borderWidth?: number;
  borderTopWidth?: number;
  borderRightWidth?: number;
  borderBottomWidth?: number;
  borderLeftWidth?: number;
  borderColor?: string;
  borderTopColor?: string;
  borderRightColor?: string;
  borderBottomColor?: string;
  borderLeftColor?: string;
  borderStyle?: 'solid' | 'dashed' | 'dotted';

  // Transform.
  transform?: TransformStyle[];
  transformOrigin?: TransformOrigin;

  // Shadow. Rendered only in a build with shadows enabled (ERUI_SHADOWS) and `shadowOpacity` above 0.
  shadowColor?: string;
  shadowOffset?: ShadowOffset;
  shadowOpacity?: number;
  shadowRadius?: number;
  elevation?: number;
}

export interface TextStyle extends ViewStyle {
  color?: string | AnimatedValue;
  fontSize?: number;
  fontFamily?: string;
  /** Anything from 600 up (or 'bold') selects the bold face; the engine carries no other weights. */
  fontWeight?: string | number;
  /** 'italic' is a synthetic slant (horizontal shear), not a separate face. */
  fontStyle?: 'normal' | 'italic';
  textAlign?: 'auto' | 'left' | 'right' | 'center';
  textDecorationLine?: 'none' | 'underline' | 'line-through';
  lineHeight?: number;
  letterSpacing?: number;
}

/** <Modal> styling: a View plus the scrim colour painted behind the modal. */
export interface ModalStyle extends ViewStyle {
  backdropColor?: string;
}

/** <TextInput> styling: text styling plus the caret colour. */
export interface TextInputStyle extends TextStyle {
  cursorColor?: string;
}

export type StyleProp<T> = T | false | null | undefined | StyleProp<T>[];

// --- Events ----------------------------------------------------------------

/** The laid-out box reported by `onLayout`, in parent coordinates. */
export interface LayoutRectangle {
  x: number;
  y: number;
  width: number;
  height: number;
}

/**
 * The object an event handler receives. `type` is the engine's own name for the event that fired, so a
 * handler shared between events can switch on it. Coordinates are screen px; `dx`/`dy` are the distance
 * traveled since the touch went down.
 */
/**
 * The touch payload every gesture callback receives: the point, the displacement from touch-down, and
 * the speed the finger was traveling at when it was last measured. Raw `onTouch*` events carry the same
 * fields as the responder events do, so a flick is `onTouchEnd={e => e.vx > 0.4 && next()}` — no
 * PanResponder and no clock needed.
 */
export interface TouchPoint {
  x: number;
  y: number;
  dx: number;
  dy: number;
  /** Velocity in px/ms, measured over the most recent move (0 before the first one). */
  vx: number;
  vy: number;
}

export interface NativeEvent<T extends string = string> extends TouchPoint {
  type: T;
}

/** The `type` string on each of the engine's responder lifecycle events. */
export type ResponderEvent = NativeEvent<
  | 'responderGrant'
  | 'responderReject'
  | 'responderMove'
  | 'responderRelease'
  | 'responderTerminate'
>;

/**
 * What a responder NEGOTIATION query is handed. Deliberately a bare `TouchPoint`: a query is not one
 * of the named events — the engine asks it mid-hit-test — so the object carries NO `type` field. Reading
 * `event.type` in a should-set predicate would be `undefined` at runtime, and the types say so.
 */
export type ResponderQueryEvent = TouchPoint;

/** Any touch or press event — the type to give a handler shared across several of them. */
export type GestureResponderEvent = NativeEvent<
  | 'press'
  | 'longPress'
  | 'pressIn'
  | 'pressOut'
  | 'touchStart'
  | 'touchMove'
  | 'touchEnd'
  | 'touchCancel'
>;

/** Any <TextInput> focus / blur / submit event. */
export type TextInputEvent = NativeEvent<'focus' | 'blur' | 'submitEditing'>;

export interface ScrollEvent extends NativeEvent<'scroll'> {
  scrollX: number;
  scrollY: number;
}

export interface LayoutChangeEvent extends NativeEvent<'layout'> {
  layout: LayoutRectangle;
}

/**
 * The gesture responder system, RN-shaped — accepted on every host component. The `*ShouldSet*` props
 * are negotiation QUERIES: on every touch-down (start) and touch-move (move) the engine walks the hit
 * chain — capture phase root→leaf first, then bubble leaf→root — and the first node whose predicate
 * returns true claims the gesture. The `onResponder*` events then fire on the owner alone. A granted
 * responder blocks a ScrollView ancestor's auto-scroll and takes the gesture back from one that has
 * already started scrolling. Most apps want `PanResponder` (below) instead of wiring these directly.
 */
export interface GestureResponderHandlers {
  onStartShouldSetResponder?: (event: ResponderQueryEvent) => boolean;
  onStartShouldSetResponderCapture?: (event: ResponderQueryEvent) => boolean;
  onMoveShouldSetResponder?: (event: ResponderQueryEvent) => boolean;
  onMoveShouldSetResponderCapture?: (event: ResponderQueryEvent) => boolean;
  /** The gesture was won: this node now owns the touch stream. */
  onResponderGrant?: (event: ResponderEvent) => void;
  /** A claim was refused — the current owner declined to yield. Re-asked on later moves. */
  onResponderReject?: (event: ResponderEvent) => void;
  /** A move while owning the gesture; `dx`/`dy` accumulate from touch-down. */
  onResponderMove?: (event: ResponderEvent) => void;
  /** The touch lifted while this node owned the gesture. */
  onResponderRelease?: (event: ResponderEvent) => void;
  /** The gesture was taken away (a canceled touch, or this node yielded to a challenger). */
  onResponderTerminate?: (event: ResponderEvent) => void;
  /** A challenger wants the gesture: return true to yield (the default), false to keep it. */
  onResponderTerminationRequest?: (event: ResponderQueryEvent) => boolean;
}

/**
 * Touch and layout handlers, accepted on every host component. Raw touches bubble from the node that was
 * hit up through its ancestors, so a handler on a container sees its children's touches too — and they
 * keep bubbling whatever the responder system decides; the responder props above are how a node OWNS a
 * gesture. `onTouchCancel` fires when the sequence is abandoned rather than finished — the host reporting
 * a canceled touch, or a fresh touch-down on a finger whose previous sequence never ended — and is the
 * hook for undoing whatever `onTouchStart` began.
 */
export interface TouchEventProps extends GestureResponderHandlers {
  onTouchStart?: (event: NativeEvent<'touchStart'>) => void;
  onTouchMove?: (event: NativeEvent<'touchMove'>) => void;
  onTouchEnd?: (event: NativeEvent<'touchEnd'>) => void;
  onTouchCancel?: (event: NativeEvent<'touchCancel'>) => void;
  onLayout?: (event: LayoutChangeEvent) => void;
}

/**
 * The press family. A press looks for the nearest ancestor carrying one of these, so wrapping a subtree in
 * a single <Pressable> is enough — no handler per child.
 */
export interface PressEventProps {
  onPress?: (event: NativeEvent<'press'>) => void;
  onLongPress?: (event: NativeEvent<'longPress'>) => void;
  onPressIn?: (event: NativeEvent<'pressIn'>) => void;
  onPressOut?: (event: NativeEvent<'pressOut'>) => void;
}

// --- Components ------------------------------------------------------------

export interface ViewProps extends TouchEventProps {
  style?: StyleProp<ViewStyle>;
  children?: ReactNode;
  /** Prunes the subtree from layout, raster and hit-testing without unmounting it (style `display` wins). */
  visible?: boolean;
  ref?: Ref<NodeHandle>;
}

export interface TextProps extends TouchEventProps {
  style?: StyleProp<TextStyle>;
  children?: ReactNode;
  /** Truncate past this many lines. 0 (the default) does not truncate. */
  numberOfLines?: number;
  ellipsizeMode?: 'head' | 'middle' | 'tail' | 'clip';
  visible?: boolean;
  ref?: Ref<NodeHandle>;
}

/**
 * An image source: the name an asset import resolves to (the bundler's image plugin turns
 * `import logo from './logo.png'` into the file's basename), or an RN-style `{uri}`. RN's numeric
 * `require()` id is not accepted — it carries no engine-side asset name, so it never resolves.
 */
export type ImageSource = string | {uri: string};

export interface ImageProps extends TouchEventProps {
  source: ImageSource;
  style?: StyleProp<ViewStyle>;
  /** How the bitmap fills its box. Defaults to 'cover'. */
  resizeMode?: 'cover' | 'contain' | 'stretch' | 'repeat' | 'center';
  /** Recolors the image, keeping its alpha — for monochrome icons. */
  tintColor?: string;
  /** The engine asset name, when set directly instead of through `source`. */
  imageName?: string;
  visible?: boolean;
  ref?: Ref<NodeHandle>;
}

export interface PressableProps extends TouchEventProps, PressEventProps {
  style?: StyleProp<ViewStyle>;
  children?: ReactNode;
  disabled?: boolean;
  visible?: boolean;
  ref?: Ref<NodeHandle>;
}

/**
 * A scrolling container. It scrolls whichever axis overflows, so lay the content out with
 * `flexDirection: 'row'` for a horizontal scroller.
 */
export interface ScrollViewProps extends ViewProps {
  onScroll?: (event: ScrollEvent) => void;
}

/**
 * `<FlatList>` is a thin `<ScrollView>` alias, **not** a virtualized list — every row mounts as a real
 * engine node and stays mounted. These four props are the only ones either flow honors (the AOT
 * rejects the rest at compile time); for headers, footers, separators, `horizontal` or
 * `onEndReached`, use a `<ScrollView>` with `.map` directly.
 */
export interface FlatListProps<T = unknown> {
  data?: readonly T[];
  renderItem?: (info: {item: T; index: number}) => ReactNode;
  keyExtractor?: (item: T, index: number) => string | number;
  style?: StyleProp<ViewStyle>;
}

/**
 * `<SectionList>` is a thin `<ScrollView>` alias like `<FlatList>` — no virtualization, no sticky
 * headers, and every header, row and footer mounts as a real engine node and stays mounted. Header,
 * rows and footer are flat siblings, exactly as in RN. Flow A / simulator only for now: the AOT has no
 * `<SectionList>` lowering yet.
 */
export interface SectionListProps<ItemT = unknown, SectionT = DefaultSectionT> {
  sections?: ReadonlyArray<SectionListData<ItemT, SectionT>>;
  renderItem?: (info: SectionListRenderItemInfo<ItemT, SectionT>) => ReactNode;
  renderSectionHeader?: (info: {
    section: SectionListData<ItemT, SectionT>;
  }) => ReactNode;
  renderSectionFooter?: (info: {
    section: SectionListData<ItemT, SectionT>;
  }) => ReactNode;
  keyExtractor?: (item: ItemT, index: number) => string | number;
  style?: StyleProp<ViewStyle>;
}

/** Whatever else a section carries alongside its `data` — RN's convention is a `title`. */
export type DefaultSectionT = {[key: string]: any};

/** One section: your own fields, plus the row data and the optional per-section overrides. */
export type SectionListData<ItemT, SectionT = DefaultSectionT> = SectionT & {
  data: readonly ItemT[];
  /** Namespaces this section's row keys. Defaults to the section's index. */
  key?: string | number;
  /** Overrides the list-wide `renderItem` for this section only. */
  renderItem?: (info: SectionListRenderItemInfo<ItemT, SectionT>) => ReactNode;
  /** Overrides the list-wide `keyExtractor` for this section only. */
  keyExtractor?: (item: ItemT, index: number) => string | number;
};

/** What `renderItem` is called with. RN's `separators` is absent — there are no separators here. */
export interface SectionListRenderItemInfo<ItemT, SectionT = DefaultSectionT> {
  item: ItemT;
  /** The row's index WITHIN its section. */
  index: number;
  section: SectionListData<ItemT, SectionT>;
}

/**
 * RN's pre-styled button: a `<Pressable>` around a single centred `<Text>`. It takes no `style` — that
 * is upstream's design, and the escape hatch is the same one RN gives you: build the `<Pressable>` +
 * `<Text>` yourself. Accessibility, TV-focus and `testID` props are accepted and ignored (no OS to
 * report them to); anything else warns once.
 */
export interface ButtonProps {
  title?: string;
  onPress?: (event: NativeEvent<'press'>) => void;
  /** Fill colour, replacing the default blue. */
  color?: string;
  /** Greys the button out and detaches `onPress`. */
  disabled?: boolean;
}

/**
 * A `<View>` with an `<Image>` stretched behind its children — what you reach for because `<Image>`
 * takes no children. `style` lays out the container; every other prop goes to the image, as upstream.
 */
export interface ImageBackgroundProps extends Omit<
  ImageProps,
  'style' | 'ref'
> {
  /** Container style — the box the picture fills (its content box, so padding insets the picture). */
  style?: StyleProp<ViewStyle>;
  /** Merged over the fill: `borderRadius`, `opacity`, `tintColor`. */
  imageStyle?: StyleProp<ViewStyle>;
  /** Ref to the `<Image>` node; the outer `ref` points at the `<View>`. */
  imageRef?: Ref<NodeHandle>;
  children?: ReactNode;
  ref?: Ref<NodeHandle>;
}

export interface TextInputProps extends TouchEventProps {
  style?: StyleProp<TextInputStyle>;
  value?: string;
  placeholder?: string;
  placeholderTextColor?: string;
  editable?: boolean;
  visible?: boolean;
  onChangeText?: (text: string) => void;
  onSubmitEditing?: (event: NativeEvent<'submitEditing'>) => void;
  onFocus?: (event: NativeEvent<'focus'>) => void;
  onBlur?: (event: NativeEvent<'blur'>) => void;
  ref?: Ref<NodeHandle>;
}

export interface SwitchProps extends TouchEventProps {
  style?: StyleProp<ViewStyle>;
  value?: boolean;
  onValueChange?: (value: boolean) => void;
  trackColor?: {false?: string; true?: string};
  thumbColor?: string;
  visible?: boolean;
  ref?: Ref<NodeHandle>;
}

export interface ActivityIndicatorProps extends TouchEventProps {
  /** The spinner tint comes from the style's `color`, as in RN. */
  style?: StyleProp<TextStyle>;
  /** Whether the spinner is turning. Defaults to true. */
  animating?: boolean;
  visible?: boolean;
  ref?: Ref<NodeHandle>;
}

export interface ModalProps extends TouchEventProps {
  style?: StyleProp<ModalStyle>;
  children?: ReactNode;
  /** Shows or hides the modal. Unlike other components this does not mean style `display`. */
  visible?: boolean;
  ref?: Ref<NodeHandle>;
}

export interface DialGradient {
  /** 'conic' sweeps the stops along the angle; 'radial' across the thickness. */
  type: 'conic' | 'radial';
  stops: Array<{color: string; offset?: number}>;
}

/**
 * Native arc widget: a dial / gauge / progress ring rasterized analytically by the engine. Angles are
 * degrees clockwise from 3 o'clock; the default is a 270° sweep starting at 135° (the LVGL arc).
 */
export interface DialProps extends TouchEventProps {
  style?: StyleProp<ViewStyle>;
  /** Current value in [min, max]. Animatable with Animated.Value (native driver). */
  value?: number | AnimatedValue;
  min?: number;
  max?: number;
  startAngle?: number;
  sweepAngle?: number;
  /** Drag quantisation step (default 1). */
  step?: number;
  /** Track + indicator thickness in px (default: 1/10 of the smaller side). */
  thickness?: number;
  /** Optional wider backing band behind the track, centred on it. */
  bandThickness?: number;
  bandColor?: string;
  trackColor?: string;
  indicatorColor?: string;
  /** Indicator paint ramp; overrides indicatorColor. */
  indicatorGradient?: DialGradient;
  cap?: 'butt' | 'round';
  /** Split the arc into N segments separated by gapAngle degrees (default 2). */
  segments?: number;
  gapAngle?: number;
  /** 'child': the engine positions the first child on the value point (multi-knob dials, custom knobs). */
  knob?: 'none' | 'circle' | 'image' | 'child';
  knobSize?: number;
  knobColor?: string;
  knobBorderColor?: string;
  knobBorderWidth?: number;
  knobImage?: ImageSource;
  /** Built-in drag-to-set: the knob follows the finger natively; onChange gets the quantized value. */
  adjustable?: boolean;
  /**
   * Dual-setpoint mode: the indicator spans [valueStart, value] and gets a knob at each end. A drag
   * latches the end it started nearest, and the ends clamp against each other instead of crossing.
   */
  range?: boolean;
  /** RANGE mode: the band's low end. Animatable like `value`. */
  valueStart?: number | AnimatedValue;
  /**
   * RANGE mode: the minimum separation the two ends keep, in value units. 0 (default) lets them meet.
   * Above 0, a drag that would close the gap pushes the far end along instead of stopping dead, and
   * stops only once that end reaches the range bound.
   */
  minSpan?: number;
  /** Called with the new value; in `range` mode the band's low end is the second argument. */
  onChange?: (value: number, valueStart: number) => void;
  visible?: boolean;
  children?: ReactNode;
  ref?: Ref<NodeHandle>;
}

export const View: (props: ViewProps) => JSX.Element;
export const Text: (props: TextProps) => JSX.Element;
export const Image: (props: ImageProps) => JSX.Element;
export const Pressable: (props: PressableProps) => JSX.Element;
export const TouchableOpacity: (props: PressableProps) => JSX.Element;
export const ScrollView: (props: ScrollViewProps) => JSX.Element;
export function FlatList<T>(props: FlatListProps<T>): JSX.Element;
export function SectionList<ItemT, SectionT = DefaultSectionT>(
  props: SectionListProps<ItemT, SectionT>,
): JSX.Element;
export const Button: (props: ButtonProps) => JSX.Element;
export const ImageBackground: (props: ImageBackgroundProps) => JSX.Element;
export const TextInput: (props: TextInputProps) => JSX.Element;
export const Switch: (props: SwitchProps) => JSX.Element;
export const ActivityIndicator: (props: ActivityIndicatorProps) => JSX.Element;
export const Modal: (props: ModalProps) => JSX.Element;
export const Dial: (props: DialProps) => JSX.Element;

// --- SVG -------------------------------------------------------------------
// <Svg> is the only host node (an engine vector node). The shape tags are descriptive children that the
// renderer flattens into that node's op-tape — like raw text inside <Text>, they are never mounted on
// their own, so they take no style, no events and no ref. Use them only inside an <Svg>.

/** An SVG geometry attribute: a number, or a numeric string as in the SVG markup ('12', '1.5'). */
export type SvgNumber = number | string;

/** Gradient kind: 1 linear (axis (ax,ay)→(bx,by)), 2 radial (centre (ax,ay), radius r), 3 conic. */
export type VectorGradientType = 1 | 2 | 3;

/**
 * A gradient ramp for a shape's fill or stroke. Up to 8 stops are carried to the engine; a longer list is
 * resampled down to 8 rather than truncated, so the ramp keeps its shape.
 */
export interface VectorGradient {
  type: VectorGradientType;
  stops: Array<{color: string | number; offset: number}>;
  ax?: number;
  ay?: number;
  bx?: number;
  by?: number;
  r?: number;
}

/**
 * Paint attributes, shared by every shape. A <G> resolves these for its children, which override
 * individually. Colours accept #rgb / #rgba / #rrggbb / #rrggbbaa, rgb() / rgba(), and a small named set
 * (none, transparent, black, white, red, green, blue, gray). An unfilled shape defaults to BLACK — set
 * `fill="none"` on anything meant to be stroke-only.
 */
export interface SvgPaintProps {
  fill?: string;
  stroke?: string;
  strokeWidth?: SvgNumber;
  strokeLinecap?: 'butt' | 'round' | 'square';
  strokeLinejoin?: 'miter' | 'round' | 'bevel';
  strokeMiterlimit?: SvgNumber;
  fillRule?: 'nonzero' | 'evenodd';
  /** Fills with a gradient instead of the solid `fill`. */
  fillGrad?: VectorGradient;
  /** Strokes with a gradient instead of the solid `stroke`. */
  strokeGrad?: VectorGradient;
}

/** A `.svg` import: the build bakes it to a vector op-tape, or to a raster asset when it uses features the
 *  vector baker cannot represent (text, masks, filters). Pass it straight to `<Svg source>`. */
export type SvgSource =
  | {
      kind: 'vector';
      ops: number[];
      paints: number[];
      gradients?: unknown[];
      width: number;
      height: number;
    }
  | {kind: 'raster'; name: string; width: number; height: number};

export interface SvgProps extends SvgPaintProps, TouchEventProps {
  children?: ReactNode;
  /** An imported `.svg`. Its tape is scaled to the render box and any shape children are ignored. */
  source?: SvgSource;
  /** Render-box size. Taken as direct props (the react-native-svg convention); a style width/height wins. */
  width?: SvgNumber;
  height?: SvgNumber;
  /** 'minX minY width height'. Coordinates are baked into the tape against width/height. */
  viewBox?: string;
  style?: StyleProp<ViewStyle>;
  visible?: boolean;
  ref?: Ref<NodeHandle>;
}

/** Groups shapes under one paint and an optional translate/scale, composed with any enclosing <G>. */
export interface GProps extends SvgPaintProps {
  children?: ReactNode;
  x?: SvgNumber;
  y?: SvgNumber;
  translateX?: SvgNumber;
  translateY?: SvgNumber;
  scale?: SvgNumber;
}

export interface PathProps extends SvgPaintProps {
  /** Path data. M/L/H/V/C/S/Q/T/A/Z, absolute and relative. Parsing a `d` string costs more than the
   *  primitive shapes — prefer <Circle>/<Rect>/<Arc> for anything rebuilt per frame. */
  d?: string;
}

export interface CircleProps extends SvgPaintProps {
  cx?: SvgNumber;
  cy?: SvgNumber;
  r?: SvgNumber;
}

export interface EllipseProps extends SvgPaintProps {
  cx?: SvgNumber;
  cy?: SvgNumber;
  rx?: SvgNumber;
  ry?: SvgNumber;
}

export interface RectProps extends SvgPaintProps {
  x?: SvgNumber;
  y?: SvgNumber;
  width?: SvgNumber;
  height?: SvgNumber;
  /** Corner radii. Omitting one (or giving it a negative value, which SVG treats as `auto`) falls back to
   *  the other; each is clamped to half its own side. */
  rx?: SvgNumber;
  ry?: SvgNumber;
}

export interface LineProps extends SvgPaintProps {
  x1?: SvgNumber;
  y1?: SvgNumber;
  x2?: SvgNumber;
  y2?: SvgNumber;
}

/**
 * Circular-arc convenience primitive (not a standard SVG element). Angles are DEGREES clockwise from
 * 12 o'clock. It emits a native arc op — no `d` parsing or bezier conversion — so it is cheap enough to
 * rebuild every drag frame.
 */
export interface ArcProps extends SvgPaintProps {
  cx?: SvgNumber;
  cy?: SvgNumber;
  r?: SvgNumber;
  startAngle?: SvgNumber;
  endAngle?: SvgNumber;
}

export const Svg: (props: SvgProps) => JSX.Element;
export const Path: (props: PathProps) => JSX.Element;
export const Circle: (props: CircleProps) => JSX.Element;
export const Ellipse: (props: EllipseProps) => JSX.Element;
export const Rect: (props: RectProps) => JSX.Element;
export const Line: (props: LineProps) => JSX.Element;
export const G: (props: GProps) => JSX.Element;
export const Arc: (props: ArcProps) => JSX.Element;

// --- StyleSheet ------------------------------------------------------------
export const StyleSheet: {
  create<T extends Record<string, ViewStyle | TextStyle>>(styles: T): T;
};

// --- Platform --------------------------------------------------------------
export const Platform: {
  OS: string;
  select<T>(specifics: Record<string, T>): T | undefined;
};

// --- AppRegistry -----------------------------------------------------------
export const AppRegistry: {
  registerComponent(
    appKey: string,
    componentProvider: () => (props: any) => JSX.Element,
  ): void;
};

// --- Animated --------------------------------------------------------------
// A handle to an engine-side animated float. Not importable directly — obtain one from `useAnimatedValue`
// or `new Animated.Value(...)`, so this is a type, not a runtime export.
export interface AnimatedValue {
  setValue(value: number): void;
  interpolate(config: {
    inputRange: number[];
    outputRange: number[] | string[];
  }): AnimatedValue;
}

export interface AnimationConfig {
  toValue: number;
  duration?: number;
  delay?: number;
  easing?: (t: number) => number;
  useNativeDriver?: boolean;
}

export interface Animation {
  start(callback?: (result: {finished: boolean}) => void): void;
  stop?(): void;
}

export const Animated: {
  Value: new (initial?: number) => AnimatedValue;
  View: (props: ViewProps) => JSX.Element;
  Text: (props: TextProps) => JSX.Element;
  Image: (props: ImageProps) => JSX.Element;
  timing(value: AnimatedValue, config: AnimationConfig): Animation;
  spring(value: AnimatedValue, config: AnimationConfig): Animation;
  decay(value: AnimatedValue, config: Record<string, unknown>): Animation;
  sequence(animations: Animation[]): Animation;
  parallel(
    animations: Animation[],
    config?: {stopTogether?: boolean},
  ): Animation;
  stagger(delay: number, animations: Animation[]): Animation;
  loop(animation: Animation, config?: {iterations?: number}): Animation;
  delay(ms: number): Animation;
};

/** Creates an AnimatedValue tied to the component lifecycle (destroyed on unmount). */
export function useAnimatedValue(initial?: number): AnimatedValue;

/** Like useState, but the value survives a dev hot reload. */
export function usePersistentState<S>(
  initialState: S | (() => S),
): [S, (value: S | ((prev: S) => S)) => void];

export function useHostValue(initial: number): number;
export const Easing: Record<string, (t: number) => number>;
export const LayoutAnimation: Record<string, unknown>;

// --- PanResponder ----------------------------------------------------------

/**
 * The running state of a drag, handed to every PanResponder callback. It is ONE mutable object reused
 * for the responder's lifetime — read what you need during the callback; don't stash it expecting a
 * snapshot.
 */
export interface PanResponderGestureState {
  /** Identifies this responder; stable for its lifetime. */
  stateID: number;
  /** Latest touchpoint. */
  moveX: number;
  moveY: number;
  /** Where the gesture was granted — the anchor `dx`/`dy` are measured from. */
  x0: number;
  y0: number;
  /** Travel since the grant, in pixels. */
  dx: number;
  dy: number;
  /** Velocity in pixels per millisecond. */
  vx: number;
  vy: number;
  /** Fingers currently on the panel. */
  numberActiveTouches: number;
}

export type PanResponderCallback = (
  event: ResponderEvent,
  gestureState: PanResponderGestureState,
) => void;

/**
 * Returning `true` claims the gesture — or, for a termination request, KEEPS it. The event is a bare
 * touchpoint: a negotiation query carries no `type`.
 */
export type PanResponderPredicate = (
  event: ResponderQueryEvent,
  gestureState: PanResponderGestureState,
) => boolean;

/**
 * RN's PanResponder config, riding the engine's native responder system (the low-level
 * GestureResponderHandlers above). Returning `true` from a should-set predicate claims the gesture
 * through real negotiation — capture asks root→leaf before bubble asks leaf→root — so a granted pan
 * OWNS the touch stream: a ScrollView ancestor will not auto-scroll under it, and a move-should-set
 * claim takes the gesture back from a scroller that already started. With no should-set supplied the
 * responder never grants, exactly as in RN.
 *
 * Only `onShouldBlockNativeResponder` (Android-specific) is absent; passing it — or a typo — warns.
 *
 * Flow B compiles this config too: the AOT lowers `useRef(PanResponder.create({…})).current` and the
 * `{...pan.panHandlers}` spread onto the same engine responder system, so the gesture needs no JS on the
 * device. `onPanResponderStart`/`End` are the exception — they fold EXTRA fingers into one gesture, which
 * is Flow A only, and the AOT fails the build by name rather than dropping them.
 */
export interface PanResponderConfig {
  onStartShouldSetPanResponder?: PanResponderPredicate;
  onStartShouldSetPanResponderCapture?: PanResponderPredicate;
  onMoveShouldSetPanResponder?: PanResponderPredicate;
  onMoveShouldSetPanResponderCapture?: PanResponderPredicate;
  onPanResponderGrant?: PanResponderCallback;
  onPanResponderMove?: PanResponderCallback;
  onPanResponderRelease?: PanResponderCallback;
  /** The gesture was taken away: a canceled touch, or this responder yielded to a challenger. */
  onPanResponderTerminate?: PanResponderCallback;
  /** A challenger wants the gesture: return true to yield (the default), false to keep it. */
  onPanResponderTerminationRequest?: PanResponderPredicate;
  /** This responder's claim lost a negotiation. Re-asked claims are re-rejected on later moves. */
  onPanResponderReject?: PanResponderCallback;
  /** A later finger joined the gesture in flight. Flow A only (the AOT rejects it by name). */
  onPanResponderStart?: PanResponderCallback;
  /**
   * A finger lifted, but the gesture continues on the ones still down. Driven by the raw touch stream
   * rather than a responder event, so this one receives the `touchEnd` payload. Flow A only (the AOT
   * rejects it by name).
   */
  onPanResponderEnd?: (
    event: NativeEvent<'touchEnd'>,
    gestureState: PanResponderGestureState,
  ) => void;
}

export interface PanResponderInstance {
  /** Spread onto any component: `<View {...pan.panHandlers} />`. */
  panHandlers: TouchEventProps;
}

/**
 * Recognizes drags, swipes, and flings via the native gesture responder system. Create it ONCE per
 * component and keep it in a ref — the handlers close over one gesture state, so re-creating it per
 * render loses the drag.
 */
export const PanResponder: {
  create(config?: PanResponderConfig): PanResponderInstance;
};

// --- Imperative escape hatch -----------------------------------------------
// For continuous gestures, where a React render per pointer move is too slow. Grab a node handle from a
// ref, push updates here, and commit back to React state when the gesture ends.

/**
 * One primitive for `updateVector`: exactly one geometry key plus its paint. Note the SHORT paint spellings
 * (`cap`, `join`, `miter`) — this is the allocation-free path, not the JSX attribute set.
 */
export interface VectorShape {
  /** [cx, cy, r, startDeg, endDeg] — degrees clockwise from 12 o'clock. */
  arc?: [number, number, number, number, number];
  /** [cx, cy, r] */
  circle?: [number, number, number];
  /** [x1, y1, x2, y2] */
  line?: [number, number, number, number];
  /** [x, y, w, h], or [x, y, w, h, rx, ry] for rounded corners. */
  rect?:
    | [number, number, number, number]
    | [number, number, number, number, number, number];
  /** Path data. Slower than the primitives above (regex + bezier conversion). */
  path?: string;
  fill?: string;
  stroke?: string;
  strokeWidth?: number;
  miter?: number;
  cap?: 'butt' | 'round' | 'square';
  join?: 'miter' | 'round' | 'bevel';
  fillRule?: 'nonzero' | 'evenodd';
  fillGrad?: VectorGradient;
  strokeGrad?: VectorGradient;
}

/**
 * Sets an <Svg> node's geometry from primitive shape descriptors, skipping React and the `d` parser.
 * `dirtyRect` is an optional node-local [x, y, w, h] bounding the change; give it and only that region is
 * repainted, which is a large win for a small update on a big vector node.
 */
export function updateVector(
  handle: NodeHandle | null | undefined,
  shapes: VectorShape[],
  dirtyRect?: [number, number, number, number],
): void;

/** Sets a <Text> node's content without disturbing its style. A later React render reverts cleanly. */
export function updateText(
  handle: NodeHandle | null | undefined,
  text: string | number,
): void;

/** One key in a `setKeyboardConfig` layer. `char` types it; `layer` switches layer; `backspace`/`done` act. */
export interface KeyboardKey {
  char?: string;
  label?: string;
  /** Index of the layer this key switches to. */
  layer?: number;
  backspace?: boolean;
  done?: boolean;
  /** Grid columns the key spans (default 1) — a wider space bar or shift. */
  span?: number;
  /** Lit while its own layer is showing. */
  highlight?: boolean;
}

/** A keyboard layout: layers of rows of keys. Omit `layers` to keep the built-in QWERTY. */
export interface KeyboardConfig {
  panelColor?: string;
  keyColor?: string;
  keyActiveColor?: string;
  labelColor?: string;
  fontSize?: number;
  rowHeight?: number;
  keyGap?: number;
  keyRadius?: number;
  gridCols?: number;
  layers?: KeyboardKey[][][];
}

/**
 * Customizes the on-screen software keyboard. Only effective when the engine was built with the keyboard
 * enabled (ERUI_ONSCREEN_KEYBOARD=1); a no-op otherwise. Pass nothing to restore the built-in default.
 */
export function setKeyboardConfig(config?: KeyboardConfig | null): void;
