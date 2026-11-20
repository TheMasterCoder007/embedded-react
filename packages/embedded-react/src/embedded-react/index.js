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

// Public 'embedded-react' surface — the React Native analog. App authors import components, styling,
// and the entry point from here; React hooks still come from 'react' (as in RN).
//
//   import { useState } from 'react';
//   import { View, Text, StyleSheet, AppRegistry } from 'embedded-react';
//
// Resolves as a Node package self-reference (see package.json "name"/"exports").
export {
  View,
  Text,
  Image,
  ScrollView,
  TextInput,
  Switch,
  ActivityIndicator,
  Modal,
  Dial,
  Svg,
  Path,
  Circle,
  Ellipse,
  Rect,
  Line,
  G,
  Arc,
  SvgText,
  ClipPath,
  Mask,
} from './components.mts';
export {Pressable} from './Pressable.mts';
export {Button} from './Button.mts';
export {TouchableOpacity} from './TouchableOpacity.mts';
export {FlatList} from './FlatList.mts';
export {ImageBackground} from './ImageBackground.mts';
export {SectionList} from './SectionList.mts';
export {
  updateVector,
  updateText,
  scrollTo,
  setKeyboardConfig,
} from './imperative.mts';
export {StyleSheet} from './StyleSheet.mts';
export {Platform} from './Platform.mts';
export {Dimensions, useWindowDimensions} from './Dimensions.mts';
export {AppRegistry} from './AppRegistry.mts';
export {Alert} from './Alert.mts';
export {Keyboard} from './Keyboard.mts';
export {Animated, useAnimatedValue} from './Animated.mts';
export {PanResponder} from './PanResponder.mts';
export {usePersistentState} from './usePersistentState.mts';
export {useHostValue} from './useHostValue.mts';
export {nativeModule, useNativeEvent} from './nativeModule.mts';
export {Easing} from './Easing.mts';
export {LayoutAnimation} from './LayoutAnimation.mts';
