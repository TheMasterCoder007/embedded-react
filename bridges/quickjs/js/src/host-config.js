/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// react-reconciler host config: translates React's mutation API into NativeUI.* calls.
//
// Instances ARE the integer node handles returned by NativeUI.createNode(). We keep no JS-side
// wrapper objects — the engine owns the scene graph, the handle is the identity.
import { DefaultEventPriority } from 'react-reconciler/constants';
import { NativeUI } from './native-ui.js';
import { buildProps, buildTextSpans, isEventProp, isTextContent } from './props.js';
import { flattenSvg, warnVectorCaps, scaleVectorArtifact, encodeVectorGradients } from './embedded-react/svg-ops.js';
import { splitAnimatedStyle } from './embedded-react/split-style.js';

/**
 * Applies a node's resolved props, binding any Animated.Value found in its `style` to the matching
 * node prop (native driver). This makes animated styles work on ANY host element — `<Pressable
 * style={{ transform: [{ scale: v }] }}>` binds without an Animated.* wrapper — which is what the
 * Flow B AOT compiler does too, so the two render paths stay in parity. An Animated.* wrapper has
 * already stripped its bindings into a ref, so splitAnimatedStyle finds none here (no double bind).
 */
function applyProps(type, handle, props) {
  const { staticStyle, bindings } = splitAnimatedStyle(props.style);
  NativeUI.setProps(handle, buildProps(type, bindings.length ? { ...props, style: staticStyle } : props));
  for (const b of bindings) b.value.__bind(handle, b.prop);
}

/**
 * Applies inline-styled text spans for a <Text> node (no-op for other types). buildTextSpans returns
 * [] for uniform text, which reverts the node to plain-text rendering — so this also clears stale
 * spans when a Text changes from styled-runs to a single style across renders.
 */
function applyTextSpans(type, handle, props) {
  if (type === 'Text') NativeUI.setTextSpans(handle, buildTextSpans(props));
}

/** Resolves an <Svg>'s render-box dimension from style/props, falling back to the source's intrinsic size. */
function svgBoxSize(props, dim, intrinsic) {
  const s = props.style && typeof props.style[dim] === 'number' ? props.style[dim] : undefined;
  const p = typeof props[dim] === 'number' ? props[dim] : undefined;
  return s ?? p ?? intrinsic;
}

/**
 * A `<Svg source={imported}>` whose imported .svg fell back to a RASTER image at build time (the SVG used
 * features the vector baker can't represent). Returns the artifact, or null for a vector/declarative
 * Svg. Such a Svg is rendered as an IMAGE node, not a vector node — `props.source.kind` is the discriminator
 * (an imported artifact's kind is fixed at build time, so it never flips for a given element).
 */
function rasterSvgArtifact(type, props) {
  const src = props && props.source;
  return type === 'Svg' && src && src.kind === 'raster' ? src : null;
}

/** Maps a raster `<Svg source>` to equivalent `<Image>` props: the baked asset name + the resolved box size. */
function rasterImageProps(props, art) {
  const width = svgBoxSize(props, 'width', art.width);
  const height = svgBoxSize(props, 'height', art.height);
  return { ...props, source: art.name, style: { ...(props.style || {}), width, height } };
}

/**
 * Sets an <Svg>'s vector op-tape. Two sources, source prop wins:
 *   <Svg source={imported}>  — an imported .svg's baked artifact ({kind:'vector', ops, paints, width,
 *                              height}); we scale its op-tape from intrinsic px to the node's box.
 *   <Svg><Path/>...</Svg>    — declarative children flattened here (the Svg owns its subtree; React does
 *                              not mount the shape children, so we compile them on create + every update).
 */
function applyVectorOps(type, handle, props) {
  if (type !== 'Svg') return;
  let ops;
  let paints;
  let gradients;
  const src = props.source;
  if (src && src.kind === 'vector' && Array.isArray(src.ops)) {
    ({ ops, paints, gradients } = scaleVectorArtifact(src, svgBoxSize(props, 'width', src.width), svgBoxSize(props, 'height', src.height)));
  } else {
    ({ ops, paints } = flattenSvg(props));
  }
  warnVectorCaps(ops.length, paints.length, NativeUI.maxVectorOps, NativeUI.maxVectorPaints, gradients ? gradients.length : 0, NativeUI.maxVectorGrads);
  NativeUI.setVectorOps(handle, ops, paints, gradients && gradients.length ? encodeVectorGradients(gradients) : undefined);
}

/**
 * Decides whether a committed <Svg> update actually needs its op-tape re-uploaded.
 *
 * Re-marshaling a baked vector op-tape across the JS->C bridge every frame is expensive (it dominates an
 * interactive drag on PSRAM-QuickJS), and it's pure waste when only the node's POSITION changed: a
 * `<Svg source>` whose imported artifact and resolved box are unchanged renders identical geometry, and its
 * on-screen movement is already handled by the layout props (left/top/width/height) via applyProps. So we
 * re-upload only when the source artifact reference changes or the resolved box size changes. Declarative
 * `<Svg><Path/></Svg>` has no `source` — its shapes live in props/children, which we can't cheaply diff
 * here, so it always re-flattens (unchanged behavior).
 */
function vectorNeedsUpload(type, prevProps, nextProps) {
  if (type !== 'Svg') return false;
  if (!nextProps.source) return true; // declarative children: re-flatten as before
  if (!prevProps || prevProps.source !== nextProps.source) return true;
  const src = nextProps.source;
  const iw = src && src.kind === 'vector' ? src.width : undefined;
  const ih = src && src.kind === 'vector' ? src.height : undefined;
  return (
    svgBoxSize(prevProps, 'width', iw) !== svgBoxSize(nextProps, 'width', iw) ||
    svgBoxSize(prevProps, 'height', ih) !== svgBoxSize(nextProps, 'height', ih)
  );
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
    // A raster-fallback <Svg source> becomes a real Image node (the SVG was rasterized at build time).
    const raster = rasterSvgArtifact(type, props);
    if (raster) {
      const handle = NativeUI.createNode('Image');
      applyProps('Image', handle, rasterImageProps(props, raster));
      applyEvents(handle, null, props);
      return handle;
    }
    const handle = NativeUI.createNode(type);
    applyProps(type, handle, props);
    applyTextSpans(type, handle, props);
    applyVectorOps(type, handle, props);
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
    // <Svg> also owns its subtree: the shape children are flattened into the vector op-tape
    // (applyVectorOps), never mounted as host nodes.
    return type === 'Svg' || (type === 'Text' && isTextContent(props.children));
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
    const raster = rasterSvgArtifact(type, nextProps);
    if (raster) {
      // The Svg instance is an Image node (raster fallback); re-apply as image props, never vector ops.
      applyProps('Image', instance, rasterImageProps(nextProps, raster));
      applyEvents(instance, prevProps, nextProps);
      return;
    }
    applyProps(type, instance, nextProps);
    applyTextSpans(type, instance, nextProps);
    if (vectorNeedsUpload(type, prevProps, nextProps)) applyVectorOps(type, instance, nextProps);
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
