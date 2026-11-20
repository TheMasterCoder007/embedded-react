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
// prop list (`PASSTHROUGH` in src/props.mts), and the SVG compiler (src/embedded-react/svg-ops.mts). A prop
// the engine ignores is deliberately absent, so a name that typechecks is a name that reaches the engine.
// `npm test` runs a parity test over those tables to keep it that way.
//
// Flow B (the AOT compiler) lowers a documented SUBSET of this surface and fails the build on anything it
// cannot compile, so these types describe Flow A and the AOT reports the difference.

import type {ComponentType, ReactNode, Ref} from 'react';

/** A node handle: what a `ref` on a host component receives, and what the imperative API takes. */
export type NodeHandle = number;

// --- Styling ---------------------------------------------------------------

/**
 * A box dimension. Percentage strings are resolved for `width`, `height`, `flexBasis` and the insets —
 * every other box property (margins, padding, min/max, borders) takes pixels and ignores a string.
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
 * One transform entry. Every axis except `perspective` can be driven by an animated value; `scale` binds
 * both axes at once. A rotation is a CSS angle string ('45deg', '0.5rad') or a number of degrees.
 */
export type TransformStyle =
  | {scale: number | AnimatedNode}
  | {scaleX: number | AnimatedNode}
  | {scaleY: number | AnimatedNode}
  | {translateX: number | AnimatedNode}
  | {translateY: number | AnimatedNode}
  | {rotate: string | number | AnimatedNode}
  | {rotateX: string | number | AnimatedNode}
  | {rotateY: string | number | AnimatedNode}
  | {rotateZ: string | number | AnimatedNode}
  | {perspective: number};

/**
 * Value type of an untyped style entry. Styles stay open (a not-yet-declared key is not an error), and the
 * union covers the object- and array-valued entries — `shadowOffset`, `transformOrigin`, `transform` — so
 * they are legal both under their own declarations and through the index signature.
 */
export type StyleValue =
  | string
  | number
  | AnimatedNode
  | TransformStyle[]
  | BackgroundImageValue[]
  | ShadowOffset
  | TransformOrigin
  | undefined;

/** A color stop of a gradient. Each position is a percentage such as `'40%'`; two paint the color across a band. */
export interface GradientColorStop {
  color: string;
  positions?: string[];
}

/**
 * A gradient in React Native's object form. `direction` is an angle (`'45deg'`) or `to` a side or corner
 * (`'to bottom right'`), and defaults to `'to bottom'`. A radial gradient is a circle from the center to the
 * farthest corner.
 */
export type BackgroundImageValue =
  | {
      type: 'linear-gradient';
      direction?: string;
      colorStops: GradientColorStop[];
    }
  | {
      type: 'radial-gradient';
      shape: 'circle';
      size?: 'farthest-corner';
      position?: 'center';
      colorStops: GradientColorStop[];
    };

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

  // Insets (with `position: 'absolute'`). A percentage measures against the containing block:
  // left/right against its width, top/bottom against its height.
  top?: DimensionValue;
  left?: DimensionValue;
  right?: DimensionValue;
  bottom?: DimensionValue;

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
  flexBasis?: DimensionValue;
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
  backgroundColor?: string | AnimatedNode;
  /**
   * One gradient of two to four stops behind a View, ScrollView or Pressable: a CSS `linear-gradient()` or
   * `radial-gradient(circle, ...)` string, or an array of one gradient object. Firmware built without
   * `ERUI_GRADIENT` paints `backgroundColor` instead.
   */
  backgroundImage?: string | BackgroundImageValue[];
  opacity?: number | AnimatedNode;

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

  // Shadow. Rendered only in a build with shadows enabled (ERUI_SHADOWS), by `shadowOpacity` above 0 or an `elevation`.
  shadowColor?: string;
  shadowOffset?: ShadowOffset;
  shadowOpacity?: number;
  shadowRadius?: number;
  elevation?: number;
}

export interface TextStyle extends ViewStyle {
  color?: string | AnimatedNode;
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

/** <Modal> styling: a View plus the scrim color painted behind the modal. */
export interface ModalStyle extends ViewStyle {
  backdropColor?: string;
}

/** <TextInput> styling: text styling plus the caret color. */
export interface TextInputStyle extends TextStyle {
  cursorColor?: string;
}

export type StyleProp<T> = T | false | null | undefined | StyleProp<T>[];

// --- Events ----------------------------------------------------------------

/** The laid-out box reported by `onLayout`. `x`/`y` are relative to the parent, as in React Native. */
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
  /** The same rect, where React Native puts it. */
  nativeEvent: {layout: LayoutRectangle};
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
  delayLongPress?: number;
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
 * A `<Pressable>` that dims while it is held — every Pressable prop works here unchanged. The fade runs
 * on the native driver, so it costs no re-render; note that a dimmed node composites its whole subtree
 * off-screen, so dim the box that reads as the button rather than the surrounding screen.
 *
 * The press feedback owns `opacity`: an animated one in `style` is ignored here and rejected by the AOT.
 * To animate opacity yourself, use `<Pressable>`. Animating any other property, `transform` included, is
 * fine.
 */
export interface TouchableOpacityProps extends PressableProps {
  /** Opacity to dim to while held, 0–1. Defaults to RN's 0.2. */
  activeOpacity?: number;
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
 * rows and footer are flat siblings, exactly as in RN. Flow B unrolls the sections at build time, so
 * write them out in place or use a constant array.
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
  secureTextEntry?: boolean;
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
  visible?: boolean;
  backdropColor?: string;
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
  value?: number | AnimatedNode;
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
  valueStart?: number | AnimatedNode;
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
export const TouchableOpacity: (props: TouchableOpacityProps) => JSX.Element;
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
// Arrays and fragments around them are unwrapped, so a mapped list of shapes works. A component is never
// rendered — flattening the subtree means it is not mounted, so `<Needle/>` draws nothing whatever it
// returns; inline its shapes, or call it as a plain function ({needle()}) to put them in children.

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
  /** A color, or `url(#id)` naming a <LinearGradient> or <RadialGradient> in the same <Svg>. */
  fill?: string;
  /** A color, or `url(#id)` naming a <LinearGradient> or <RadialGradient> in the same <Svg>. */
  stroke?: string;
  /** 0 to 1, multiplied into the fill's alpha, gradients included. */
  fillOpacity?: SvgNumber;
  /** 0 to 1, multiplied into the stroke's alpha, gradients included. */
  strokeOpacity?: SvgNumber;
  /**
   * 0 to 1, multiplied into the fill and the stroke. A <G>'s multiplies into its shapes', each faded on its own,
   * so where they overlap they show through each other.
   */
  opacity?: SvgNumber;
  strokeWidth?: SvgNumber;
  strokeLinecap?: 'butt' | 'round' | 'square';
  strokeLinejoin?: 'miter' | 'round' | 'bevel';
  strokeMiterlimit?: SvgNumber;
  fillRule?: 'nonzero' | 'evenodd';
  /**
   * Dash and gap lengths along the stroke: a number, a list, or a string of them (`'6 3'`). Each dash
   * takes `strokeLinecap`. `'none'` is solid.
   */
  strokeDasharray?: SvgNumber | readonly SvgNumber[];
  /**
   * How far into its dashes the stroke starts. An animated value moves them on the native driver, with
   * no re-render; every animated one in an `<Svg>` follows the same value.
   */
  strokeDashoffset?: SvgNumber | AnimatedNode;
  /** Fills with a gradient instead of the solid `fill`. */
  fillGradient?: VectorGradient;
  /** Strokes with a gradient instead of the solid `stroke`. */
  strokeGradient?: VectorGradient;
}

/** A `.svg` import: the build bakes it to a vector op-tape, text included, or to a raster asset when it uses
 *  features the vector baker cannot represent (masks, filters). Pass it straight to `<Svg source>`. */
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
/** What a shape or <G> is clipped or masked by. */
export interface SvgMaskedProps {
  /** A <ClipPath> in the same <Svg>, as `url(#id)`: the element shows only inside its shapes. */
  clipPath?: string;
  /** A <Mask> in the same <Svg>, as `url(#id)`: the element shows as far as the mask is light. */
  mask?: string;
}

/**
 * A clip: an element with `clipPath="url(#id)"` shows only inside these shapes. It draws nothing itself,
 * and its shapes' colors are not read.
 */
export interface ClipPathProps extends SvgMaskedProps {
  id?: string;
  children?: ReactNode;
  /** `objectBoundingBox` places the shapes across the clipped element's box, 0 to 1 each way. */
  clipPathUnits?: 'userSpaceOnUse' | 'objectBoundingBox';
  /** How a shape's area is worked out, for every shape that does not give its own. */
  clipRule?: 'nonzero' | 'evenodd';
}

/**
 * A mask: an element with `mask="url(#id)"` shows as far as these shapes are light (or opaque, with
 * `maskType="alpha"`). It draws nothing itself.
 */
export interface MaskProps extends SvgPaintProps, SvgMaskedProps {
  id?: string;
  children?: ReactNode;
  maskType?: 'luminance' | 'alpha';
  /** The box the mask stops at. Defaults to a tenth past the masked element's box on every side. */
  x?: SvgNumber;
  y?: SvgNumber;
  width?: SvgNumber;
  height?: SvgNumber;
  /** What `x`, `y`, `width` and `height` are in. Defaults to `objectBoundingBox`. */
  maskUnits?: 'userSpaceOnUse' | 'objectBoundingBox';
  /** `objectBoundingBox` places the shapes across the masked element's box, 0 to 1 each way. */
  maskContentUnits?: 'userSpaceOnUse' | 'objectBoundingBox';
}

export interface GProps extends SvgPaintProps, SvgMaskedProps {
  children?: ReactNode;
  /** Inside a <ClipPath>: how its shapes' areas are worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  x?: SvgNumber;
  y?: SvgNumber;
  translateX?: SvgNumber;
  translateY?: SvgNumber;
  scale?: SvgNumber;
}

export interface PathProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  /** Path data. M/L/H/V/C/S/Q/T/A/Z, absolute and relative. Parsing a `d` string costs more than the
   *  primitive shapes — prefer <Circle>/<Rect>/<Arc> for anything rebuilt per frame. */
  d?: string;
}

export interface CircleProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  cx?: SvgNumber;
  cy?: SvgNumber;
  r?: SvgNumber;
}

export interface EllipseProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  cx?: SvgNumber;
  cy?: SvgNumber;
  rx?: SvgNumber;
  ry?: SvgNumber;
}

export interface RectProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  x?: SvgNumber;
  y?: SvgNumber;
  width?: SvgNumber;
  height?: SvgNumber;
  /** Corner radii. Omitting one (or giving it a negative value, which SVG treats as `auto`) falls back to
   *  the other; each is clamped to half its own side. */
  rx?: SvgNumber;
  ry?: SvgNumber;
}

export interface LineProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  x1?: SvgNumber;
  y1?: SvgNumber;
  x2?: SvgNumber;
  y2?: SvgNumber;
}

/** Points as `'x1,y1 x2,y2 …'`, or as a flat list of numbers. */
export type SvgPoints = string | readonly SvgNumber[];

/** A closed shape through its points. */
export interface PolygonProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  points?: SvgPoints;
}

/** An open line through its points. */
export interface PolylineProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  points?: SvgPoints;
}

/** Holds gradients, clips and masks. Nothing in it is drawn where it sits. */
export interface DefsProps {
  children?: ReactNode;
}

/** What a gradient's coordinates are in: the painted shape's box, 0 to 1 each way (the default), or its own units. */
export type GradientUnits = 'objectBoundingBox' | 'userSpaceOnUse';

/** A gradient along a line, used as `fill="url(#id)"` or `stroke="url(#id)"`. Holds <Stop>s. */
export interface LinearGradientProps {
  id?: string;
  children?: ReactNode;
  /** Where the line starts and ends: numbers, or percentages such as `'50%'`. Defaults to across the box. */
  x1?: SvgNumber;
  y1?: SvgNumber;
  x2?: SvgNumber;
  y2?: SvgNumber;
  gradientUnits?: GradientUnits;
}

/** A gradient out from a centre, used as `fill="url(#id)"` or `stroke="url(#id)"`. Holds <Stop>s. */
export interface RadialGradientProps {
  id?: string;
  children?: ReactNode;
  /** The centre and radius: numbers, or percentages such as `'50%'`. Default to a circle filling the box. */
  cx?: SvgNumber;
  cy?: SvgNumber;
  r?: SvgNumber;
  gradientUnits?: GradientUnits;
}

/** One color along a gradient. Up to 8 per gradient. */
export interface StopProps {
  /** 0 to 1, or a percentage such as `'50%'`. A stop before the one ahead of it is moved up to it. */
  offset?: SvgNumber;
  stopColor?: string;
  /** 0 to 1, multiplied into the stop's alpha. */
  stopOpacity?: SvgNumber;
}

/**
 * Circular-arc convenience primitive (not a standard SVG element). Angles are DEGREES clockwise from
 * 12 o'clock. It emits a native arc op — no `d` parsing or bezier conversion — so it is cheap enough to
 * rebuild every drag frame.
 */
export interface ArcProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  cx?: SvgNumber;
  cy?: SvgNumber;
  r?: SvgNumber;
  startAngle?: SvgNumber;
  endAngle?: SvgNumber;
}

/**
 * Text drawn as glyph outlines, so it scales and turns with its <Svg>. The build bakes it to a path from the
 * font file, so the text and everything that places it must be constants: literals, module-level constants,
 * or an imported font. Paint may change at run time. For text that changes, put a <Text> over the <Svg>.
 */
export interface SvgTextProps extends SvgPaintProps, SvgMaskedProps {
  /** Inside a <ClipPath>: how this shape's area is worked out. */
  clipRule?: 'nonzero' | 'evenodd';
  /** The text. */
  children?: string | number;
  /** Where the baseline starts, or its middle or end by `textAnchor`. */
  x?: SvgNumber;
  y?: SvgNumber;
  /** An imported font (`import Inter from './fonts/Inter.ttf'`), or the name of a font file beside the
   *  source file or in its `fonts` folder. */
  fontFamily?: string;
  /** In px. Defaults to 16. */
  fontSize?: SvgNumber;
  /** A weight other than normal takes the family's file for it, such as `Inter-Bold.ttf`. */
  fontWeight?: 'normal' | 'bold' | SvgNumber;
  /** Extra space after each glyph, in px. */
  letterSpacing?: SvgNumber;
  textAnchor?: 'start' | 'middle' | 'end';
  /** Where `y` sits on the glyphs. Defaults to the baseline. */
  dominantBaseline?: 'auto' | 'middle' | 'central' | 'hanging';
  /** An SVG transform list, such as `rotate(90 50 50)`, baked into the outlines. */
  transform?: string;
}

export const Svg: (props: SvgProps) => JSX.Element;
export const Path: (props: PathProps) => JSX.Element;
export const Circle: (props: CircleProps) => JSX.Element;
export const Ellipse: (props: EllipseProps) => JSX.Element;
export const Rect: (props: RectProps) => JSX.Element;
export const Line: (props: LineProps) => JSX.Element;
export const Polygon: (props: PolygonProps) => JSX.Element;
export const Polyline: (props: PolylineProps) => JSX.Element;
export const G: (props: GProps) => JSX.Element;
export const Defs: (props: DefsProps) => JSX.Element;
export const LinearGradient: (props: LinearGradientProps) => JSX.Element;
export const RadialGradient: (props: RadialGradientProps) => JSX.Element;
export const Stop: (props: StopProps) => JSX.Element;
export const Arc: (props: ArcProps) => JSX.Element;
export const SvgText: (props: SvgTextProps) => JSX.Element;
export const ClipPath: (props: ClipPathProps) => JSX.Element;
export const Mask: (props: MaskProps) => JSX.Element;

// --- StyleSheet ------------------------------------------------------------
export const StyleSheet: {
  create<T extends Record<string, ViewStyle | TextStyle>>(styles: T): T;
  /** Merges a style array (nested, with falsy entries skipped) into one object; later entries win. */
  flatten(style?: StyleProp<ViewStyle>): ViewStyle;
  /** 1 — the engine draws whole pixels, so the thinnest line is one. */
  hairlineWidth: number;
  /** `{position: 'absolute', top: 0, left: 0, right: 0, bottom: 0}`. */
  absoluteFill: ViewStyle;
};

// --- Platform --------------------------------------------------------------
export const Platform: {
  OS: string;
  select<T>(specifics: Record<string, T>): T | undefined;
};

// --- Dimensions ------------------------------------------------------------
/** The panel's size in pixels. `scale` and `fontScale` are always 1. */
export interface ScaledSize {
  width: number;
  height: number;
  scale: number;
  fontScale: number;
}

/** What a `change` listener receives: the new size, under both names. */
export interface DimensionsChange {
  window: ScaledSize;
  screen: ScaledSize;
}

export const Dimensions: {
  /** The panel's size. 'window' and 'screen' are the same on a panel. */
  get(dimension: 'window' | 'screen'): ScaledSize;
  /**
   * Runs `handler` when the host changes the size while the app runs: a live rotation, or a simulator resize.
   * Flow A only: a Flow B package is compiled for its sizes.
   */
  addEventListener(
    type: 'change',
    handler: (sizes: DimensionsChange) => void,
  ): {remove(): void};
};

/** The panel's size. A component that reads it renders again when the size changes. */
export function useWindowDimensions(): ScaledSize;

// --- Alert -----------------------------------------------------------------
export interface AlertButton {
  text?: string;
  onPress?: () => void;
  /** 'destructive' draws the label in red; 'cancel' looks like 'default'. */
  style?: 'default' | 'cancel' | 'destructive';
}

export interface AlertOptions {
  /** Lets a tap outside the dialog close it. Off by default. */
  cancelable?: boolean;
  /** Called when a tap outside closes a cancelable alert. */
  onDismiss?: () => void;
}

export const Alert: {
  /**
   * Shows a stock dialog over the app. With no buttons it gets a single "OK". A second alert waits
   * until the first is closed; in Flow B it replaces the first.
   */
  alert(
    title: string,
    message?: string,
    buttons?: AlertButton[],
    options?: AlertOptions,
  ): void;
};

// --- Keyboard --------------------------------------------------------------
export type KeyboardEventName =
  | 'keyboardWillShow'
  | 'keyboardDidShow'
  | 'keyboardWillHide'
  | 'keyboardDidHide';

/** The strip of the screen the on-screen keyboard covers, in pixels. */
export interface KeyboardMetrics {
  screenX: number;
  screenY: number;
  width: number;
  height: number;
}

export interface KeyboardEvent {
  /** Where the keyboard ends up: its strip when it shows, the same strip just below the screen when it hides. */
  endCoordinates: KeyboardMetrics;
}

/**
 * The on-screen keyboard, which shows while a `<TextInput>` has the focus. A build without it
 * (ERUI_ONSCREEN_KEYBOARD=0) fires no events and is never visible; `dismiss()` still takes the focus away.
 */
export const Keyboard: {
  /** Nothing animates, so each "will" event fires just before its "did". */
  addListener(
    eventName: KeyboardEventName,
    listener: (event: KeyboardEvent) => void,
  ): {remove(): void};
  removeAllListeners(eventName: KeyboardEventName): void;
  /** Takes the focus from the focused `<TextInput>`, which hides the keyboard. */
  dismiss(): void;
  isVisible(): boolean;
  /** The keyboard's strip, or undefined while it is hidden. */
  metrics(): KeyboardMetrics | undefined;
};

// --- AppRegistry -----------------------------------------------------------
export const AppRegistry: {
  registerComponent(
    appKey: string,
    componentProvider: () => (props: any) => JSX.Element,
  ): void;
};

// --- Animated --------------------------------------------------------------
/** Behavior outside `inputRange`: keep the slope ('extend'), hold the end value, or pass the input through. */
export type ExtrapolateType = 'extend' | 'clamp' | 'identity';

/**
 * A piecewise-linear mapping, up to 8 breakpoints. `outputRange` is numbers, or colors for
 * `backgroundColor` and `color`, which take nothing else; the engine carries no unit-string
 * interpolation, and a rotation axis reads the output in degrees. `extrapolate` sets both ends and the
 * per-end keys override it.
 */
export interface InterpolationConfig {
  inputRange: number[];
  outputRange: number[] | string[];
  extrapolate?: ExtrapolateType;
  extrapolateLeft?: ExtrapolateType;
  extrapolateRight?: ExtrapolateType;
}

/** Anything a style or `Dial` value can be bound to: an `AnimatedValue` or one of its interpolations. */
export interface AnimatedNode {
  interpolate(config: InterpolationConfig): AnimatedNode;
}

// A handle to an engine-side animated float. Not importable directly — obtain one from `useAnimatedValue`
// or `new Animated.Value(...)`, so this is a type, not a runtime export.
export interface AnimatedValue extends AnimatedNode {
  setValue(value: number): void;
  /**
   * Frees the engine-side slot. The value pool is fixed-size, so a `new Animated.Value` that outlives its
   * screen must be destroyed; `useAnimatedValue` does it on unmount. Idempotent.
   */
  destroy(): void;
}

/** A named engine curve. */
export type EasingName =
  | 'linear'
  | 'ease'
  | 'easeIn'
  | 'easeOut'
  | 'easeInOut'
  | 'quadIn'
  | 'quadOut'
  | 'quadInOut'
  | 'cubicIn'
  | 'cubicOut'
  | 'cubicInOut'
  | 'bounceOut'
  | 'elasticOut';

/** A custom cubic-bezier curve, from `Easing.bezier`. */
export interface BezierEasing {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

/** An `Easing` token. The engine evaluates the curve; it is not a JS function. */
export type EasingCurve = EasingName | BezierEasing;

/** `Animated.timing` config. */
export interface AnimationConfig {
  toValue: number;
  duration?: number;
  delay?: number;
  easing?: EasingCurve;
  useNativeDriver?: boolean;
}

/** `Animated.spring` config, in the engine's physical parameters (RN's `tension`/`friction` are not read). */
export interface SpringAnimationConfig {
  toValue: number;
  /** Default 100. */
  stiffness?: number;
  /** Default 10. */
  damping?: number;
  /** Default 1. */
  mass?: number;
  velocity?: number;
  delay?: number;
  useNativeDriver?: boolean;
}

/** `Animated.decay` config. There is no target: the value coasts from `velocity`. */
export interface DecayAnimationConfig {
  /** Value units per ms. */
  velocity: number;
  /** Per-ms friction. Default 0.998. */
  deceleration?: number;
  useNativeDriver?: boolean;
}

export interface Animation {
  start(callback?: (result: {finished: boolean}) => void): void;
  stop(): void;
}

export const Animated: {
  Value: new (initial?: number) => AnimatedValue;
  View: (props: ViewProps) => JSX.Element;
  Text: (props: TextProps) => JSX.Element;
  Image: (props: ImageProps) => JSX.Element;
  timing(value: AnimatedValue, config: AnimationConfig): Animation;
  spring(value: AnimatedValue, config: SpringAnimationConfig): Animation;
  decay(value: AnimatedValue, config: DecayAnimationConfig): Animation;
  sequence(animations: Animation[]): Animation;
  parallel(
    animations: Animation[],
    config?: {stopTogether?: boolean},
  ): Animation;
  stagger(delay: number, animations: Animation[]): Animation;
  loop(
    animation: Animation,
    config?: {iterations?: number; resetBeforeIteration?: boolean},
  ): Animation;
  delay(ms: number): Animation;
  /** RN parity: a pass-through wrapper, since any component binds an animated style already. */
  createAnimatedComponent<P>(
    component: ComponentType<P>,
  ): (props: P) => JSX.Element;
};

/** Creates an AnimatedValue tied to the component lifecycle (destroyed on unmount). */
export function useAnimatedValue(initial?: number): AnimatedValue;

/** Like useState, but the value survives a dev hot reload. */
export function usePersistentState<S>(
  key: string,
  initialState: S | (() => S),
): [S, (value: S | ((prev: S) => S)) => void];

export function useHostValue(initial: number): number;

/** A whole number. A native method's `Int` is an int in C, where a `number` is a float. */
export type Int = number;

/**
 * A whole number that may be past an Int's range: a native method's `Int64` is an int64_t in C. In JS it is a number,
 * so it is exact up to 2^53.
 */
export type Int64 = number;

/** An optional native module, as nativeModule.optional returns it. */
export type OptionalNativeModule<T> = T & {
  /** Whether the firmware has every method and event of the module. Set at load, and it never changes. */
  readonly available: boolean;
};

/**
 * A module the firmware provides in C, declared at module level by an interface of its methods, which take and
 * return Int, Int64, number, boolean, string and records of them:
 * `const Sensors = nativeModule<Sensors>('sensors', 1)`. Its events are the interface's `events`:
 * `events: {reading(celsius: number): void}`.
 */
export const nativeModule: {
  <T>(name: string, version: number): T;
  /**
   * A module only some boards have: `const Wifi = nativeModule.optional<Wifi>('wifi', 1)`. A package that uses it
   * loads without it, and `Wifi.available` says whether it is there. Calling a method of a module that isn't stops
   * the app; its events never arrive.
   */
  optional<T>(name: string, version: number): OptionalNativeModule<T>;
};

/** The events a native module's interface declares. */
export type NativeEvents<T> = T extends {events: infer E} ? E : {};

/** The values an event is posted with. */
export type NativeEventValues<
  T,
  K extends keyof NativeEvents<T>,
> = NativeEvents<T>[K] extends (...values: infer V) => void ? V : never;

/**
 * Runs `handler` with an event's values each time the firmware posts it, while the component is mounted:
 * `useNativeEvent(Sensors, 'reading', celsius => setTemperature(celsius))`.
 */
export function useNativeEvent<T, K extends keyof NativeEvents<T> & string>(
  module: T,
  event: K,
  handler: (...values: NativeEventValues<T, K>) => void,
): void;

/** Posts one of a mocked module's events, as the firmware would. false while one of the mock's methods runs. */
export type NativeEmit<T> = <K extends keyof NativeEvents<T> & string>(
  event: K,
  ...values: NativeEventValues<T, K>
) => boolean;

/**
 * A native module's mock for the simulators, the default export of `<name>.mock.ts`: its methods, or a function
 * that is given `emit` and returns them.
 */
export type NativeMock<T> =
  Omit<T, 'events'> | ((emit: NativeEmit<T>) => Omit<T, 'events'>);
export const Easing: {readonly [K in EasingName]: K} & {
  bezier(x1: number, y1: number, x2: number, y2: number): BezierEasing;
};

// --- LayoutAnimation -------------------------------------------------------

/** One phase of a layout animation. */
export interface LayoutAnimationPhase {
  type?: string;
  property?: string;
  springDamping?: number;
}

/** A layout animation, in React Native's shape. The engine tweens position; `update.type` picks the curve. */
export interface LayoutAnimationConfig {
  /** Default 300. */
  duration?: number;
  type?: string;
  create?: LayoutAnimationPhase;
  update?: LayoutAnimationPhase;
  delete?: LayoutAnimationPhase;
}

export const LayoutAnimation: {
  /** Animates the layout changes of the next commit. Call it right before the state update that moves things. */
  configureNext(config?: LayoutAnimationConfig | null, done?: () => void): void;
  create(
    duration: number,
    type?: string,
    creationProp?: string,
  ): LayoutAnimationConfig;
  Types: Record<
    'spring' | 'linear' | 'easeInEaseOut' | 'easeIn' | 'easeOut' | 'keyboard',
    string
  >;
  Properties: Record<'opacity' | 'scaleX' | 'scaleY' | 'scaleXY', string>;
  Presets: Record<'easeInEaseOut' | 'linear' | 'spring', LayoutAnimationConfig>;
  easeInEaseOut(done?: () => void): void;
  linear(done?: () => void): void;
  spring(done?: () => void): void;
};

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
  /** Dash and gap lengths along the stroke. */
  dash?: number[];
  /** How far into its dashes the stroke starts. */
  dashOffset?: number;
  fillGradient?: VectorGradient;
  strokeGradient?: VectorGradient;
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

/** Where a `<ScrollView>` is scrolled to, and the largest offset it accepts on each axis. */
export interface ScrollPosition {
  x: number;
  y: number;
  maxX: number;
  maxY: number;
}

/**
 * Scrolls a `<ScrollView>` and returns where it landed, or `undefined` when the handle is not a
 * ScrollView. The offset is clamped to the scroll range, and an axis left out stays where it is, so
 * `scrollTo(handle)` only reads the position. The range is as of the last render.
 */
export function scrollTo(
  handle: NodeHandle | null | undefined,
  offset?: {x?: number; y?: number},
): ScrollPosition | undefined;

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
  gridColumns?: number;
  layers?: KeyboardKey[][][];
}

/**
 * Customizes the on-screen software keyboard. Only effective when the engine was built with the keyboard
 * enabled (ERUI_ONSCREEN_KEYBOARD=1); a no-op otherwise. Pass nothing to restore the built-in default.
 */
export function setKeyboardConfig(config?: KeyboardConfig | null): void;
