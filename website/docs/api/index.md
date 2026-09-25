---
title: 'API reference'
description: 'Everything an app imports from embedded-react, the bridge it talks to, and the C engine underneath.'
---

An app imports one package. Its surface is deliberately the React Native one, so most of it needs no
introduction; these pages document what is here, what each thing takes, and where the embedded
version differs.

```jsx
import {useState, useEffect, useRef} from 'react'; // React's hooks, as in React Native
import {
  View,
  Text,
  Image,
  Pressable,
  ScrollView, // components
  StyleSheet,
  Animated,
  PanResponder,
  Easing, // APIs
  useAnimatedValue,
  usePersistentState,
  useHostValue, // hooks
} from 'embedded-react';
```

| Page                                     | Covers                                                                                                                                                          |
| ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [Components](./components.md)            | Every host component and its props: `View`, `Text`, `Image`, `Pressable`, `ScrollView`, the lists, `TextInput`, `Switch`, `Modal`, `Dial`, and the `Svg` family |
| [Hooks and app APIs](./hooks.md)         | `useAnimatedValue`, `usePersistentState`, `useHostValue`; `AppRegistry`, `Platform`, the `screen` global                                                        |
| [Styles](./styles.md)                    | The style properties the engine reads, colour formats, `StyleSheet`, and what an `Animated.Value` can drive                                                     |
| [Animated](./animated.md)                | Values, `timing`/`spring`/`decay`, composition, `interpolate`, `Easing`, `LayoutAnimation`                                                                      |
| [NativeUI bridge](./native-ui-bridge.md) | The `NativeUI` global the reconciler drives, and the `er_runtime` host core a Flow A firmware calls                                                             |
| [C engine](./c-engine.md)                | `er_scene.h`: nodes, props, commit, events, animation, assets, and the backend struct                                                                           |

Two conventions run through the reference:

- **A prop that typechecks reaches the engine.** The package's type declarations list only what the
  runtime honours; a React Native prop the engine ignores is deliberately absent, and `npm test`
  keeps the types and the bridge's tables in step. If your editor accepts a prop, it does something.
- **Flow B notes.** Where the ahead-of-time compiler accepts less than Flow A, the page says so
  next to the prop. [The AOT subset](../guides/aot-subset.md) has the full list.

Hooks such as `useState` and `useEffect` come from `react`, exactly as in React Native; the package
adds only the three hooks above.
