// AppRegistry — the RN entry point. An app registers a root component; the runtime mounts it into
// a screen-sized container.
//
// In React Native the native side calls runApplication when the activity starts. Our host model is
// "running the bundle IS starting the app", so registerComponent mounts immediately into a root
// sized from the host-injected `screen` global. (A host-driven runApplication can be split out
// later when the C host owns app lifecycle.)
import { createElement } from 'react';
import { createRoot } from '../renderer.js';

let registered = null;

export const AppRegistry = {
  /**
   * Registers a root component and mounts it. `componentProvider` is a zero-arg function returning
   * the component (RN signature), so the component module isn't evaluated until registration.
   */
  registerComponent(appKey, componentProvider) {
    registered = { appKey, Component: componentProvider() };
    AppRegistry.runApplication(appKey);
    return appKey;
  },

  /**
   * Mounts the registered root component into a fresh screen-sized container.
   */
  runApplication(appKey) {
    if (!registered) return;
    if (appKey && appKey !== registered.appKey) return;
    const root = createRoot({ width: screen.width, height: screen.height });
    root.render(createElement(registered.Component));
  },
};
