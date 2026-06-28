import {AppRegistry} from 'embedded-react';
import {App} from './App';

// Entry point: mount <App/> into a screen-sized root. `npm run dev` runs it in the WASM simulator with hot
// reload; on real hardware the same App.tsx runs via the C engine (Flow A on QuickJS, or Flow B compiled to C).
AppRegistry.registerComponent('__APP_NAME__', () => App);
