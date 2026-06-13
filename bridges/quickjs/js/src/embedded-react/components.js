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

// Host components. Each is the string tag the reconciler passes to NativeUI.createNode(), which
// maps it onto an ERNodeType. JSX `<View/>` → createElement('View', ...) → createInstance('View').
export const View = 'View';
export const Text = 'Text';
export const Image = 'Image';
export const ScrollView = 'ScrollView';
export const FlatList = 'FlatList';
export const Pressable = 'Pressable';
// TouchableOpacity is RN's press-with-opacity wrapper; for now it maps to the same Pressable node.
export const TouchableOpacity = 'Pressable';
export const TextInput = 'TextInput';
export const Switch = 'Switch';
export const ActivityIndicator = 'ActivityIndicator';
export const Modal = 'Modal';

// SVG surface. <Svg> is the only host node (→ ER_NODE_VECTOR); the shape tags below are descriptive
// children that the host config flattens into the Svg node's op-tape (they are never mounted on their
// own — like raw text inside <Text>). Use them only inside an <Svg>.
export const Svg = 'Svg';
export const Path = 'Path';
export const Circle = 'Circle';
export const Ellipse = 'Ellipse';
export const Rect = 'Rect';
export const Line = 'Line';
export const G = 'G';
// Arc convenience primitive (not a standard SVG element): a circular arc given a center, radius, and
// start/end angles in DEGREES clockwise from 12 o'clock. Flattens to a native arc op — cheap to update.
export const Arc = 'Arc';
