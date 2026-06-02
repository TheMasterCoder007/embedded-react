// react-reconciler host config: translates React's mutation API into NativeUI.* calls.
//
// Instances ARE the integer node handles returned by NativeUI.createNode(). We keep no JS-side
// wrapper objects — the engine owns the scene graph, the handle is the identity.
import { DefaultEventPriority } from 'react-reconciler/constants';
import { NativeUI } from './native-ui.js';
import { buildProps, buildTextSpans, isEventProp, isTextContent } from './props.js';

/**
 * Applies inline-styled text spans for a <Text> node (no-op for other types). buildTextSpans returns
 * [] for uniform text, which reverts the node to plain-text rendering — so this also clears stale
 * spans when a Text changes from styled-runs to a single style across renders.
 */
function applyTextSpans(type, handle, props) {
  if (type === 'Text') NativeUI.setTextSpans(handle, buildTextSpans(props));
}

/**
 * Registers/clears on* event handlers. A handler present in old but not new props is cleared.
 */
function applyEvents(handle, prevProps, nextProps) {
  if (prevProps) {
    for (const key in prevProps) {
      if (isEventProp(key, prevProps[key]) && !(nextProps && isEventProp(key, nextProps[key]))) {
        NativeUI.setEvent(handle, key, null);
      }
    }
  }
  for (const key in nextProps) {
    if (isEventProp(key, nextProps[key])) {
      NativeUI.setEvent(handle, key, nextProps[key]);
    }
  }
}

export const hostConfig = {
  supportsMutation: true,
  supportsPersistence: false,
  supportsHydration: false,
  isPrimaryRenderer: true,
  noTimeout: -1,
  warnsIfNotActing: false,

  // --- Context (we carry none) ---
  getRootHostContext() {
    return {};
  },
  getChildHostContext(parentContext) {
    return parentContext;
  },
  getPublicInstance(instance) {
    return instance;
  },

  // --- Commit lifecycle ---
  prepareForCommit() {
    return null;
  },
  resetAfterCommit() {
    NativeUI.commit();
  },

  // --- Creation ---
  createInstance(type, props) {
    const handle = NativeUI.createNode(type);
    NativeUI.setProps(handle, buildProps(type, props));
    applyTextSpans(type, handle, props);
    applyEvents(handle, null, props);
    return handle;
  },
  createTextInstance(text) {
    // Raw text is only legal inside <Text> (handled via shouldSetTextContent). This fallback
    // wraps stray text in a Text node so it still renders rather than crashing.
    const handle = NativeUI.createNode('Text');
    NativeUI.setProps(handle, { text: String(text) });
    return handle;
  },
  appendInitialChild(parent, child) {
    NativeUI.appendChild(parent, child);
  },
  finalizeInitialChildren() {
    return false;
  },
  shouldSetTextContent(type, props) {
    // Own the whole subtree for any flattenable <Text> (strings, interpolation, nested <Text>): React
    // skips mounting children and we render them via the node's text + spans. Non-flattenable content
    // (e.g. a <View> inside <Text>) returns false, falling back to mounted child instances.
    return type === 'Text' && isTextContent(props.children);
  },

  // --- Mutation ---
  appendChild(parent, child) {
    NativeUI.appendChild(parent, child);
  },
  appendChildToContainer(container, child) {
    NativeUI.appendChild(container, child);
  },
  insertBefore(parent, child, beforeChild) {
    NativeUI.insertBefore(parent, child, beforeChild);
  },
  insertInContainerBefore(container, child, beforeChild) {
    NativeUI.insertBefore(container, child, beforeChild);
  },
  removeChild(parent, child) {
    NativeUI.removeChild(parent, child);
    NativeUI.destroyNode(child);
  },
  removeChildFromContainer(container, child) {
    NativeUI.removeChild(container, child);
    NativeUI.destroyNode(child);
  },
  clearContainer() {
    // Children are removed individually via removeChildFromContainer.
  },
  prepareUpdate() {
    // Always re-apply: NativeUI.setProps is fully declarative, so a non-null payload is enough.
    return true;
  },
  commitUpdate(instance, _payload, type, prevProps, nextProps) {
    NativeUI.setProps(instance, buildProps(type, nextProps));
    applyTextSpans(type, instance, nextProps);
    applyEvents(instance, prevProps, nextProps);
  },
  commitTextUpdate(textInstance, _oldText, newText) {
    NativeUI.setProps(textInstance, { text: String(newText) });
  },

  // --- Misc required hooks (no-ops for our renderer) ---
  detachDeletedInstance() {},
  getCurrentEventPriority() {
    return DefaultEventPriority;
  },
  getInstanceFromNode() {
    return null;
  },
  beforeActiveInstanceBlur() {},
  afterActiveInstanceBlur() {},
  prepareScopeUpdate() {},
  getInstanceFromScope() {
    return null;
  },

  // --- Scheduling ---
  scheduleTimeout: (fn, delay) => setTimeout(fn, delay),
  cancelTimeout: (id) => clearTimeout(id),
  supportsMicrotasks: true,
  scheduleMicrotask:
    typeof queueMicrotask === 'function' ? queueMicrotask : (fn) => Promise.resolve().then(fn),
  now: () => NativeUI.now(),
};
