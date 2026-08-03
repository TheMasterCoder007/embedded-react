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
import {DefaultEventPriority} from 'react-reconciler/constants';
import {NativeUI} from './native-ui.js';
import {
  buildProps,
  buildTextSpans,
  deepEqualProps,
  isEventProp,
  isTextContent,
} from './props.js';
import {
  flattenSvg,
  warnVectorCaps,
  scaleVectorArtifact,
  encodeVectorGradients,
} from './embedded-react/svg-ops.js';
import {splitAnimatedStyle} from './embedded-react/split-style.js';

/**
 * Applies a node's resolved props, binding any Animated.Value found in its `style` to the matching
 * node prop (native driver). This makes animated styles work on ANY host element — `<Pressable
 * style={{ transform: [{ scale: v }] }}>` binds without an Animated.* wrapper — which is what the
 * Flow B AOT compiler does too, so the two render paths stay in parity. An Animated.* wrapper has
 * already stripped its bindings into a ref, so splitAnimatedStyle finds none here (no double bind).
 *
 * Returns the resolved flat style so a paired applyTextSpans call can reuse it instead of
 * re-flattening props.style from scratch — style flattening (recursive array walk and object merge)
 * is one of the costlier steps in commitUpdate's JS-side work on a bytecode-interpreted MCU target.
 */
function applyProps(type, handle, props) {
  const {staticStyle, bindings} = splitAnimatedStyle(props.style);
  NativeUI.setProps(handle, buildProps(type, props, staticStyle));
  for (const b of bindings) b.value.__bind(handle, b.prop);
  return staticStyle;
}

/**
 * Applies inline-styled text spans for a <Text> node (no-op for other types). buildTextSpans returns
 * [] for uniform text, which reverts the node to plain-text rendering — so this also clears stale
 * spans when a Text changes from styled-runs to a single style across renders. `flatStyle`, when
 * given, is the already-resolved style from a paired applyProps call on the same props (avoids a
 * second full flattening of props.style).
 */
function applyTextSpans(type, handle, props, flatStyle) {
  if (type === 'Text')
    NativeUI.setTextSpans(handle, buildTextSpans(props, flatStyle));
}

/** Resolves an <Svg>'s render-box dimension from style/props, falling back to the source's intrinsic size. */
function svgBoxSize(props, dim, intrinsic) {
  const s =
    props.style && typeof props.style[dim] === 'number'
      ? props.style[dim]
      : undefined;
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
  return {
    ...props,
    source: art.name,
    style: {...(props.style || {}), width, height},
  };
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
    ({ops, paints, gradients} = scaleVectorArtifact(
      src,
      svgBoxSize(props, 'width', src.width),
      svgBoxSize(props, 'height', src.height),
    ));
  } else {
    ({ops, paints, gradients} = flattenSvg(props));
  }
  warnVectorCaps(
    ops.length,
    paints.length,
    NativeUI.maxVectorOps,
    NativeUI.maxVectorPaints,
    gradients ? gradients.length : 0,
    NativeUI.maxVectorGrads,
  );
  NativeUI.setVectorOps(
    handle,
    ops,
    paints,
    gradients && gradients.length
      ? encodeVectorGradients(gradients)
      : undefined,
  );
}

/**
 * True when a `<Svg source={vectorArtifact}>`'s resolved render box changed between renders — the
 * one style-driven input to the op-tape (the tape is scaled from the artifact's intrinsic px to the
 * box, see scaleVectorArtifact). A style change that leaves the box alone (a position move during a
 * drag, a color tweak) does not need the tape re-uploaded.
 */
function svgSourceBoxChanged(prevProps, nextProps) {
  const src = nextProps.source;
  const iw = src && src.kind === 'vector' ? src.width : undefined;
  const ih = src && src.kind === 'vector' ? src.height : undefined;
  return (
    svgBoxSize(prevProps, 'width', iw) !== svgBoxSize(nextProps, 'width', iw) ||
    svgBoxSize(prevProps, 'height', ih) !== svgBoxSize(nextProps, 'height', ih)
  );
}

/**
 * Diffs a re-rendered host element's props and returns what commitUpdate must re-apply — or null
 * when nothing observable changed, which makes React skip the commit entirely (no Update effect, no
 * bridge traffic). Re-marshaling props across the JS->C bridge is the dominant cost of a Flow A
 * re-render, and a parent's state change re-renders every non-memoized child with identical props,
 * so the null case is the common one on interactive screens.
 *
 * The payload is a set of section flags, so a partial change re-applies only its section:
 *   props  — NativeUI.setProps (resolved style + passthrough props + text)
 *   spans  — inline text spans (<Text> only; derived from children + style)
 *   vector — the <Svg> op-tape upload (flattenSvg/scaleVectorArtifact + setVectorOps)
 *   events — on* handler registration (an inline closure has a new identity every render and must be
 *            re-stored — its captures went stale — but that alone shouldn't re-serialize the props)
 *
 * The vector flag tracks the op-tape's actual inputs. A source artifact tape depends on the source
 * reference and the resolved box; a declarative <Svg><Path/></Svg> tape depends on the shape
 * children, viewBox, and the direct width/height props (flattenSvg never reads style). So a
 * position-only style move of either kind — the interactive-drag hot path — skips the upload.
 *
 * Props buildProps never reads still set the props flag when they change — conservative, and only
 * paid when such a value actually changed.
 */
function diffUpdate(type, prevProps, nextProps) {
  let props = false;
  let spans = false;
  let events = false;
  let vector = false;
  const diffKey = key => {
    const a = prevProps[key];
    const b = nextProps[key];
    if (Object.is(a, b)) return;
    if (isEventProp(key, a) || isEventProp(key, b)) {
      events = true;
      return;
    }
    switch (key) {
      case 'children':
        // Only subtree-owning types render children through their OWN props (shouldSetTextContent);
        // everywhere else children are separate host instances React mutates directly.
        if (type === 'Text') {
          if (!deepEqualProps(a, b)) {
            props = true; // flat.text is built from children
            spans = true;
          }
        } else if (type === 'Svg') {
          if (!deepEqualProps(a, b)) vector = true;
        }
        return;
      case 'style':
        if (!deepEqualProps(a, b)) {
          props = true;
          if (type === 'Text') spans = true;
          if (type === 'Svg' && nextProps.source)
            vector = vector || svgSourceBoxChanged(prevProps, nextProps);
        }
        return;
      case 'source':
        props = true; // imageName (Image / raster Svg fallback)
        if (type === 'Svg') vector = true; // artifact swap → new tape
        return;
      case 'width':
      case 'height':
      case 'viewBox':
        if (type === 'Svg') {
          props = true; // width/height fold into the resolved style box
          vector = true; // and drive the tape's root transform / artifact scale
          return;
        }
      // fall through: on other types these are ordinary (ignored or passthrough) props
      default:
        if (!deepEqualProps(a, b)) props = true;
    }
  };
  for (const key in prevProps) diffKey(key);
  for (const key in nextProps) {
    if (!Object.prototype.hasOwnProperty.call(prevProps, key)) diffKey(key);
  }
  return props || spans || events || vector
    ? {props, spans, events, vector}
    : null;
}

/**
 * Registers/clears on* event handlers. A handler present in old but not new props is cleared.
 */
function applyEvents(handle, prevProps, nextProps) {
  if (prevProps) {
    for (const key in prevProps) {
      if (
        isEventProp(key, prevProps[key]) &&
        !(nextProps && isEventProp(key, nextProps[key]))
      ) {
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
    const flatStyle = applyProps(type, handle, props);
    applyTextSpans(type, handle, props, flatStyle);
    applyVectorOps(type, handle, props);
    applyEvents(handle, null, props);
    return handle;
  },
  createTextInstance(text) {
    // Raw text is only legal inside <Text> (handled via shouldSetTextContent). This fallback
    // wraps stray text in a Text node so it still renders rather than crashing.
    const handle = NativeUI.createNode('Text');
    NativeUI.setProps(handle, {text: String(text)});
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
  prepareUpdate(instance, type, oldProps, newProps) {
    // Runs in the render phase. null → React schedules no Update effect and commitUpdate never
    // fires for this node — the win that makes an unchanged re-render bridge-free.
    return diffUpdate(type, oldProps, newProps);
  },
  commitUpdate(instance, payload, type, prevProps, nextProps) {
    const raster = rasterSvgArtifact(type, nextProps);
    if (raster) {
      // The Svg instance is an Image node (raster fallback); re-apply as image props, never vector ops.
      if (payload.props)
        applyProps('Image', instance, rasterImageProps(nextProps, raster));
      if (payload.events) applyEvents(instance, prevProps, nextProps);
      return;
    }
    // payload.spans is only ever set alongside payload.props for Text nodes (see diffUpdate), so
    // flatStyle is defined whenever applyTextSpans actually needs it.
    let flatStyle;
    if (payload.props) flatStyle = applyProps(type, instance, nextProps);
    if (payload.spans) applyTextSpans(type, instance, nextProps, flatStyle);
    if (payload.vector) applyVectorOps(type, instance, nextProps);
    if (payload.events) applyEvents(instance, prevProps, nextProps);
  },
  commitTextUpdate(textInstance, _oldText, newText) {
    NativeUI.setProps(textInstance, {text: String(newText)});
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
  cancelTimeout: id => clearTimeout(id),
  supportsMicrotasks: true,
  scheduleMicrotask:
    typeof queueMicrotask === 'function'
      ? queueMicrotask
      : fn => Promise.resolve().then(fn),
  now: () => NativeUI.now(),
};
