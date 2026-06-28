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

// Public type declarations for the `embedded-react` package — the React Native analog. These cover the
// common surface (View / Text / Pressable / Image / StyleSheet / Animated / hooks); the SVG primitives are
// typed loosely for now. The runtime is JavaScript and ignores types entirely — these only power editors
// and `tsc`. React hooks (useState, useEffect, …) still come from 'react', as in React Native.

import type {ReactNode} from 'react';

// --- Styling ---------------------------------------------------------------
/** A single style object. Properties mirror the React Native subset the engine supports. */
export interface ViewStyle {
  [key: string]: string | number | undefined | TransformStyle[];
  flex?: number;
  flexDirection?: 'row' | 'column' | 'row-reverse' | 'column-reverse';
  alignItems?: 'flex-start' | 'flex-end' | 'center' | 'stretch';
  justifyContent?:
    | 'flex-start'
    | 'flex-end'
    | 'center'
    | 'space-between'
    | 'space-around'
    | 'space-evenly';
  width?: number | string;
  height?: number | string;
  maxWidth?: number | string;
  maxHeight?: number | string;
  margin?: number;
  marginTop?: number;
  marginBottom?: number;
  marginLeft?: number;
  marginRight?: number;
  padding?: number;
  paddingVertical?: number;
  paddingHorizontal?: number;
  gap?: number;
  backgroundColor?: string;
  borderRadius?: number;
  borderWidth?: number;
  borderColor?: string;
  opacity?: number;
  transform?: TransformStyle[];
}

export interface TextStyle extends ViewStyle {
  color?: string;
  fontSize?: number;
  fontFamily?: string;
  fontWeight?: string | number;
  textAlign?: 'auto' | 'left' | 'right' | 'center';
}

export type TransformStyle =
  | {scale: number | AnimatedValue}
  | {scaleX: number | AnimatedValue}
  | {scaleY: number | AnimatedValue}
  | {translateX: number | AnimatedValue}
  | {translateY: number | AnimatedValue}
  | {rotate: string | AnimatedValue};

export type StyleProp<T> = T | false | null | undefined | StyleProp<T>[];

// --- Components ------------------------------------------------------------
export interface ViewProps {
  style?: StyleProp<ViewStyle>;
  children?: ReactNode;
}

export interface TextProps {
  style?: StyleProp<TextStyle>;
  children?: ReactNode;
  numberOfLines?: number;
}

export interface PressableProps {
  style?: StyleProp<ViewStyle>;
  children?: ReactNode;
  onPress?: () => void;
  disabled?: boolean;
}

export type ImageSource = number | string | {uri: string};

export interface ImageProps {
  source: ImageSource;
  style?: StyleProp<ViewStyle>;
}

export interface ScrollViewProps extends ViewProps {
  horizontal?: boolean;
}

export interface TextInputProps {
  style?: StyleProp<TextStyle>;
  value?: string;
  placeholder?: string;
  onChangeText?: (text: string) => void;
}

export interface SwitchProps {
  value?: boolean;
  onValueChange?: (value: boolean) => void;
}

export const View: (props: ViewProps) => JSX.Element;
export const Text: (props: TextProps) => JSX.Element;
export const Image: (props: ImageProps) => JSX.Element;
export const Pressable: (props: PressableProps) => JSX.Element;
export const TouchableOpacity: (props: PressableProps) => JSX.Element;
export const ScrollView: (props: ScrollViewProps) => JSX.Element;
export const FlatList: (props: Record<string, unknown>) => JSX.Element;
export const TextInput: (props: TextInputProps) => JSX.Element;
export const Switch: (props: SwitchProps) => JSX.Element;
export const ActivityIndicator: (props: ViewProps) => JSX.Element;
export const Modal: (props: ViewProps) => JSX.Element;

// SVG primitives (see the repo for their full prop sets).
export const Svg: (props: Record<string, unknown>) => JSX.Element;
export const Path: (props: Record<string, unknown>) => JSX.Element;
export const Circle: (props: Record<string, unknown>) => JSX.Element;
export const Ellipse: (props: Record<string, unknown>) => JSX.Element;
export const Rect: (props: Record<string, unknown>) => JSX.Element;
export const Line: (props: Record<string, unknown>) => JSX.Element;
export const G: (props: Record<string, unknown>) => JSX.Element;
export const Arc: (props: Record<string, unknown>) => JSX.Element;

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
  parallel(animations: Animation[], config?: {stopTogether?: boolean}): Animation;
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

export const Easing: Record<string, (t: number) => number>;
export const LayoutAnimation: Record<string, unknown>;
export function updateVector(...args: unknown[]): void;
export function updateText(...args: unknown[]): void;
export function setKeyboardConfig(...args: unknown[]): void;
