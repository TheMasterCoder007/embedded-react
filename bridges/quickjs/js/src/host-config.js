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

// --- Animated bindings ---------------------------------------------------------------------------
// A bound prop is owned by the engine from then on: it re-pushes the animated value over the static
// props of every later setProps (er_anim_reapply_bound), which is what keeps a re-render from jumping
// a running animation back to its declared value. Nothing in React says "this prop stopped being
// animated", so the binding has to be released explicitly or it keeps writing the prop forever — and a
// prop that swapped Animated.Values ends up written by both. Instances are bare node handles with no
// JS object to hang state on, so what we bound is remembered here: handle → (prop → animated value).
const boundValues = new Map();

/**
 * True when two animated values bind a prop the same way, so a re-render can leave the live binding
 * alone. Identity settles an Animated.Value; an interpolation is a throwaway object that
 * `value.interpolate(...)` rebuilds on every render, so it answers __bindEq for itself.
 */
function sameBinding(prev, next) {
  if (prev === next) return true;
  if (prev == null || next == null) return false;
  return typeof prev.__bindEq === 'function' && prev.__bindEq(next);
}

/** The value `bindings` (from splitAnimatedStyle) drives `prop` with, or undefined if none does. */
function bindingFor(bindings, prop) {
  for (const b of bindings) {
    if (b.prop === prop) return b.value;
  }
  return undefined;
}

/**
 * Releases the bindings a node has that its new props no longer ask for — a prop that went back to a
 * static value, or moved to a different Animated.Value.
 *
 * Runs BEFORE the node's setProps, and that order is the whole point: setProps re-pushes every value
 * still bound to the node over the static props it just wrote, so a binding released afterwards would
 * leave its last animated float sitting on the node instead of the value React just set.
 */
function releaseStaleBindings(handle, bindings) {
  const bound = boundValues.get(handle);
  if (bound === undefined) return;
  for (const [prop, value] of bound) {
    if (sameBinding(value, bindingFor(bindings, prop))) continue;
    NativeUI.animUnbind(handle, prop);
    bound.delete(prop);
  }
  if (bound.size === 0) boundValues.delete(handle);
}

/**
 * Binds the props that aren't already bound to the value asking for them. The engine ignores a
 * duplicate bind, but the bridge crossing to find that out isn't free on an MCU.
 */
function applyBindings(handle, bindings) {
  if (bindings.length === 0) return;
  let bound = boundValues.get(handle);
  for (const b of bindings) {
    if (bound !== undefined && sameBinding(bound.get(b.prop), b.value))
      continue;
    b.value.__bind(handle, b.prop);
    if (bound === undefined) {
      bound = new Map();
      boundValues.set(handle, bound);
    }
    bound.set(b.prop, b.value);
  }
}

/**
 * Forgets what a handle was bound to. The engine releases the bindings itself when a node is destroyed
 * (er_anim_unbind_node), so this only drops our record — but it has to happen, because handles are
 * recycled: a record left behind would make the next node to claim that handle look already-bound.
 * Called on destroy for the node React hands us, and again on create because destroyNode reclaims a
 * whole SUBTREE in C — React never mentions the descendants, so their handles come back from the free
 * list still carrying a record.
 */
function forgetBindings(handle) {
  boundValues.delete(handle);
}

/**
 * Applies a node's resolved props, binding any Animated.Value found in its `style` to the matching
 * node prop (native driver). This makes animated styles work on ANY host element — `<Pressable
 * style={{ transform: [{ scale: v }] }}>` binds without an Animated.* wrapper — which is what the
 * Flow B AOT compiler does too, so the two render paths stay in parity (and what an Animated.* wrapper
 * relies on: it forwards its style untouched and is bound here like any other element).
 *
 * Returns the resolved flat style so a paired applyTextSpans call can reuse it instead of
 * re-flattening props.style from scratch — style flattening (recursive array walk and object merge)
 * is one of the costlier steps in commitUpdate's JS-side work on a bytecode-interpreted MCU target.
 */
function applyProps(type, handle, props) {
  const {staticStyle, bindings} = splitAnimatedStyle(props.style);
  // <Dial value={animatedValue}>: the value is a native-driver binding (ER_PROP_ARC_VALUE), not a prop —
  // the engine ramps it with no JS per frame and damages only the swept sliver.
  if (type === 'Dial') {
    if (props.value && props.value.__animated) {
      bindings.push({prop: 'value', value: props.value});
      props = {...props, value: undefined};
    }
    if (props.valueStart && props.valueStart.__animated) {
      bindings.push({prop: 'valueStart', value: props.valueStart});
      props = {...props, valueStart: undefined};
    }
  }
  releaseStaleBindings(handle, bindings);
  NativeUI.setProps(handle, buildProps(type, props, staticStyle));
  applyBindings(handle, bindings);
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
      forgetBindings(handle);
      applyProps('Image', handle, rasterImageProps(props, raster));
      applyEvents(handle, null, props);
      return handle;
    }
    const handle = NativeUI.createNode(type);
    forgetBindings(handle);
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
    forgetBindings(child);
  },
  removeChildFromContainer(container, child) {
    NativeUI.removeChild(container, child);
    NativeUI.destroyNode(child);
    forgetBindings(child);
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
