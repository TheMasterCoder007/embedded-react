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

// Button — RN's pre-styled button, as a plain component over <Pressable> + <Text>. No engine node of
// its own:
//
//   <Button title="Save" onPress={save} />
//     renders the same tree as
//   <Pressable style={styles.button} onPress={save}><Text style={styles.text}>Save</Text></Pressable>
//
// RN nests that <Text> in a <View> inside the touchable, because its Touchable has no style of its
// own. <Pressable> here IS a styled scene node, so the View is dropped: two engine nodes per button
// instead of three, which matters against a fixed ERUI_MAX_NODES pool (44 slots on the CYD).
//
// Like RN's, this Button takes NO style prop — that is the whole point of it, and the escape hatch is
// the same: drop to <Pressable> + <Text> and style them yourself. `color` and `disabled` are the two
// knobs, exactly as upstream.
import {createElement} from 'react';
import {Pressable, Text} from './components.js';
import {createPropWarner, RN_PLATFORM_NO_OPS} from './warn-props.js';

// RN's Button styles are Platform.select({ios, android}) and Platform.OS is 'embedded', so neither
// branch applies. These are the android ones — a filled, rounded button — because a panel button
// should read as a button before it is styled, and the ios branch is bare text. RN's `elevation: 4`
// is dropped: shadows are compile-gated (ERUI_SHADOWS) and off in every board config here, so it
// would be a prop marshaled on every commit to paint nothing.
const styles = {
  button: {backgroundColor: '#2196F3', borderRadius: 2},
  buttonDisabled: {backgroundColor: '#dfdfdf'},
  text: {color: '#ffffff', textAlign: 'center', padding: 8, fontWeight: '500'},
  textDisabled: {color: '#a1a1a1'},
};

/** The props that do something here. RN's rest are OS services with nothing to wrap on an MCU. */
const SUPPORTED = ['title', 'onPress', 'color', 'disabled'];

const warnUnsupportedProps = createPropWarner(
  'Button',
  SUPPORTED,
  "it is RN's fixed-style button — for a button you style yourself, use <Pressable> + <Text> " +
    '(the same tree this renders).',
  RN_PLATFORM_NO_OPS,
);

/**
 * A pre-styled press target with a centred label.
 *
 * @param {object} [props] Missing / empty renders an unlabelled button, rather than throwing on a
 *   device where a render-time throw takes the whole app down (RN `invariant`s on a missing title).
 * @param {string} [props.title] The label. Coerced to a string; not upper-cased (RN does that on
 *   Android, and an upper-cased glyph an app never wrote is a glyph the font bake never baked).
 * @param {(event: object) => void} [props.onPress] Press handler. Not attached while `disabled`.
 * @param {string} [props.color] Fill colour, replacing the default blue.
 * @param {boolean} [props.disabled] Greys the button out and stops it responding to presses.
 * @returns {*} A <Pressable> wrapping a single <Text>.
 */
export function Button(props = {}) {
  const {title, onPress, color, disabled} = props;

  warnUnsupportedProps(props);

  const buttonStyles = [styles.button];
  const textStyles = [styles.text];
  if (color) buttonStyles.push({backgroundColor: color});
  if (disabled) {
    buttonStyles.push(styles.buttonDisabled);
    textStyles.push(styles.textDisabled);
  }

  return createElement(
    Pressable,
    {
      style: buttonStyles,
      disabled,
      // Withholding the handler is what actually disables the button: `disabled` is a Pressable prop
      // in name only (the engine has no such node flag), so a node left holding onPress still fires.
      onPress: disabled ? undefined : onPress,
    },
    createElement(
      Text,
      {style: textStyles},
      title === undefined || title === null ? '' : String(title),
    ),
  );
}
