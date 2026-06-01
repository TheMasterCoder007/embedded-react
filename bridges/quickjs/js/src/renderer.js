// Public renderer API: createRoot(...).render(<App/>).
//
// The container is a real engine View node set as the scene root; React children mount into it.
// We use LegacyRoot (synchronous) mode so the initial render flushes without depending on the
// async scheduler/timers — the host's frame loop then paints whatever the tree became.
import Reconciler from 'react-reconciler';
import { hostConfig } from './host-config.js';
import { NativeUI } from './native-ui.js';

const reconciler = Reconciler(hostConfig);

const LegacyRoot = 0;

/**
 * Creates a root bound to a screen-sized container node.
 *
 * @param {object} containerProps  Props for the container View (e.g. width/height/backgroundColor).
 * @returns {{ render: (element: any) => void }}
 */
export function createRoot(containerProps) {
  const container = NativeUI.createNode('View');
  NativeUI.setProps(container, containerProps || {});
  NativeUI.setRoot(container);

  const fiberRoot = reconciler.createContainer(
    container, // containerInfo — our root node handle
    LegacyRoot,
    null, // hydration callbacks
    false, // isStrictMode
    null, // concurrentUpdatesByDefaultOverride
    '', // identifierPrefix
    (error) => console.error('react recoverable error:', error),
    null, // transitionCallbacks
  );

  return {
    render(element) {
      reconciler.updateContainer(element, fiberRoot, null, null);
    },
  };
}
