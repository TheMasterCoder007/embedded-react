// Bundle entry point. The C host injects `screen` ({ width, height, scale }) and `NativeUI`
// before running this, then drives the frame loop after it returns.
import { createRoot } from '../src/renderer.js';
import { App } from './App.jsx';

const root = createRoot({
  width: screen.width,
  height: screen.height,
  backgroundColor: '#111927',
});

root.render(<App />);
console.log('React mounted at', screen.width + 'x' + screen.height);
