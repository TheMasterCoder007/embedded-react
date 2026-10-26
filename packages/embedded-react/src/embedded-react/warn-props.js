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

// Shared "you passed a prop this component ignores" warning for the JS-only RN wrappers (Button,
// FlatList, ImageBackground, SectionList). Those components exist for API compatibility, so an app
// ported from React Native arrives carrying props that mean nothing on a panel — and a prop that is
// silently dropped is the worst outcome, because the UI just quietly isn't what the source says.
//
// Warns ONCE per component and then stops scanning: these render on the Flow A commit path, and a
// misconfigured list re-renders on every data change. One line in the device console is a hint; one
// line per commit is a flood that pushes everything else out of the scrollback.

/**
 * RN props that wrap an OS service a bare panel does not have: a screen reader, a focus engine, a test
 * runner, a click sound. They are real RN props, they cost nothing to carry, and keeping them means a
 * component still compiles when it goes back to React Native — so a wrapper drops them without a word
 * rather than making an app strip them to silence a warning.
 *
 * Note that <FlatList> deliberately does NOT quiet these: it is the one wrapper the AOT lowers, and it
 * rejects every prop outside its supported four, so a `testID` there fails the device build. Warning
 * about it is the point.
 */
export const RN_PLATFORM_NO_OPS = [
  'accessible',
  'accessibilityLabel',
  'accessibilityHint',
  'accessibilityLanguage',
  'accessibilityRole',
  'accessibilityState',
  'accessibilityActions',
  'onAccessibilityAction',
  'importantForAccessibility',
  'testID',
  'touchSoundDisabled',
  'hasTVPreferredFocus',
  'nextFocusDown',
  'nextFocusForward',
  'nextFocusLeft',
  'nextFocusRight',
  'nextFocusUp',
];

/**
 * Builds the once-only prop warner for a component.
 *
 * @param {string} component Component name, as it appears in JSX (no angle brackets).
 * @param {string[]} supported Props the component honours. Listed in the warning as the way out.
 * @param {string} hint Why the rest are ignored / what to reach for instead. Ends the first sentence.
 * @param {string[]} [quiet] Props to ignore silently — RN props that wrap an OS service this board
 *   doesn't have (accessibility, TV focus, test ids). They are portable no-ops, not mistakes.
 * @returns {(props: object) => void} Call with the full prop object on every render.
 */
export function createPropWarner(component, supported, hint, quiet = []) {
  let warned = false;
  return props => {
    if (warned) return;
    const names = Object.keys(props).filter(
      k => !supported.includes(k) && !quiet.includes(k),
    );
    if (names.length === 0) return;
    warned = true;
    console.warn(
      `embedded-react: <${component}> ignores ${names.join(', ')} — ${hint} ` +
        `Supported here: ${supported.join(', ')}.`,
    );
  };
}
