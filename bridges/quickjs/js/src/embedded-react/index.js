// Public 'embedded-react' surface — the React Native analog. App authors import components, styling,
// and the entry point from here; React hooks still come from 'react' (as in RN).
//
//   import { useState } from 'react';
//   import { View, Text, StyleSheet, AppRegistry } from 'embedded-react';
//
// Resolves as a Node package self-reference (see package.json "name"/"exports"). Animated and Easing
// are not exported yet — they land with the Animated work (BRIDGE.md §1.4).
export {
  View,
  Text,
  Image,
  ScrollView,
  FlatList,
  Pressable,
  TouchableOpacity,
  TextInput,
  Switch,
  ActivityIndicator,
  Modal,
} from './components.js';
export { StyleSheet } from './StyleSheet.js';
export { Platform } from './Platform.js';
export { AppRegistry } from './AppRegistry.js';
export { Animated } from './Animated.js';
export { Easing } from './Easing.js';
