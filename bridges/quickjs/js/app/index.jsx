// Bundle entry point — the React Native idiom. The C host injects `screen` and `NativeUI` before
// running this; AppRegistry mounts the app into a screen-sized root.
import { AppRegistry } from 'embedded-react';
import { App } from './App.jsx';

AppRegistry.registerComponent('demo', () => App);
console.log('React mounted at', screen.width + 'x' + screen.height);
