import { AppRegistry } from 'embedded-react';
import { App } from './App.jsx';

// Flow A entry point (QuickJS): mounts App into a screen-sized root. Flow B (AOT) compiles App.jsx
// directly and provides its own entry, so it doesn't use this file.
AppRegistry.registerComponent('music-player', () => App);
