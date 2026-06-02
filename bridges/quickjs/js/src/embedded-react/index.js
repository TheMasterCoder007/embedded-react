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
export { LayoutAnimation } from './LayoutAnimation.js';
