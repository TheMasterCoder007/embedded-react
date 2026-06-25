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

import { useState, useRef } from 'react';
import { View, Text, Svg, Circle, updateVector, updateText } from 'embedded-react';
import climateFace from '../assets/climate-face.svg';

// A self-contained dial: it takes its data (value/range), dimensions (size/font sizes), and colors (theme)
// as PROPS, so it imports nothing from the app and can be reused or moved freely. (The app can't expose its
// constants through a shared module here because the Flow B AOT compiler resolves identifiers only within
// the single App.jsx it parses — so app-level constants stay there and reach the dial as props instead.)

// The dial's own intrinsic geometry — the visual arc, independent of the temperature range (which is a prop).
const SWEEP = 270; // the arc spans 270°, with a 90° gap at the bottom
const A_START = -135; // bottom-left, degrees clockwise from 12 o'clock
const DEADBAND = 0.5; // stops the Heating/Cooling status flickering when target ≈ current
const SVG_BOX = 410;

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
// value → angle (deg, clockwise from top): min → A_START, max → -A_START.
const angleForValue = (v, min, max) => A_START + ((v - min) / (max - min)) * SWEEP;
// A point on the arc. The minus on cos puts 0° at the top and grows y downward (SVG convention).
const pointOnArc = (deg, cx, cy, r) => {
  const t = (deg * Math.PI) / 180;
  return { x: cx + r * Math.sin(t), y: cy - r * Math.cos(t) };
};
// Center status word from target vs. current. The deadband stops flicker near equality.
const statusFor = (mode, value, current) => {
  if (mode === 'off') return 'Off';
  if (mode !== 'cool' && value > current + DEADBAND) return 'Heating';
  if (mode !== 'heat' && value < current - DEADBAND) return 'Cooling';
  return 'Holding';
};

const KNOB = [
  { r: 20, fill: '#ffffff' }, // white knob body
  { r: 13, fill: 'none', stroke: '#121212', sw: 3 }, // inner black ring
];
const KNOB_R_MAX = 20; // largest circle radius (authoring units) — sizes the imperative repaint box

// ----------------------------------------------------------------------------------------------------
// The arc dial — the rich Flow A (interactive) dial. The face is the imported conic SVG; a drag moves the
// knob (imperatively, no React re-render) and updates the center readout. Only the wide / stacked layouts
// render this; the compact (AOT) branch in App draws its own inline arc dial.
//
//   value    current target temperature (°F, a float)
//   min/max  the temperature range the arc spans
//   current  the live room reading (°F), shown under the number
//   mode     heat | cool | auto | off — drives the center status word
//   size     the dial's edge — a pixel number, OR a percent string ('80%', of the parent); kept square
//   sz       font sizes { sub, big } for the center readout
//   theme color tokens { subtext, text }
//   onValue  committed on release with the final continuous value
// ----------------------------------------------------------------------------------------------------
export function Dial({ value, min, max, current, mode, size, sz, theme, onValue }) {
  // `size` is either a pixel number or a percent string ('80%', relative to the parent's width). A percent
  // is only known in pixels after layout, so we measure the resolved edge in onLayout, then feed it back as
  // an explicit square height (and drive the pixel-scaled SVG geometry from it). aspectRatio can't do this
  // here: the dial's parent is a column, where the engine derives aspectRatio along the cross (width) axis,
  // not height — and skips it entirely when width is a percent. So we reserve the square ourselves; a
  // numeric size is used directly. Until the first measure a percent box has height 0 (one settle frame).
  const isPct = typeof size === 'string';
  const [measured, setMeasured] = useState(isPct ? 0 : size);
  const box = isPct ? measured : size; // the dial's edge in screen px (0 until a percent is first measured)
  const boxStyle = isPct ? { width: size, height: measured } : { width: size, height: size };

  // SVG authoring space → screen, from the pixel box.
  const DIAL_C = box / 2;
  const DIAL_R = (185 / SVG_BOX) * box; // radius (screen px) the knob travels — matches the SVG arc's radius
  const S = box / SVG_BOX; // authoring-unit → screen-px scale (for the knob circles)
  const pad = Math.max(0, Math.ceil(DIAL_R + KNOB_R_MAX * S - box / 2) + 2);
  const KNOB_C = DIAL_C + pad;

  // Dial center in absolute screen coords, captured from onLayout (a ref, so it never re-renders).
  const centerRef = useRef({ x: 0, y: 0 });
  // The value shown on screen. It drives the DECLARATIVE render (mount, ± steppers, post-drag re-sync). During
  // a drag we DON'T setShown — the knob + readout is pushed imperatively (below) so React never reconciles —
  // and commit the final value back to React state only on release.
  const [shown, setShown] = useState(value);
  const draggingRef = useRef(false);
  const lastRef = useRef(value); // latest continuous value, tracked synchronously (state batches)
  const prevKRef = useRef(null);
  const knobRef = useRef(null);
  const numRef = useRef(null);
  const statusRef = useRef(null);
  // What the center readout currently shows on screen. The drag path compares against these and only pushes a
  // text update (updateText) when the string actually changes — the number changes once per whole degree, the
  // status word rarely. Skipping the no-op updates keeps the center text box OUT of the frame's damage rect, so
  // the conic face underneath isn't re-rasterized across a center-to-rim union every move (it would be otherwise:
  // the readout is centered, the knob is at the rim, and the compositor repaints the face across their union).
  const shownIntRef = useRef(Math.round(value));
  const statusWordRef = useRef(statusFor(mode, value, current));
  // Re-sync to the committed value when not mid-drag (e.g., the ± steppers move it). Conditional
  // setState-during-render is React's idiom for "adjust state when a prop changes".
  if (!draggingRef.current && shown !== value) {
    lastRef.current = value;
    setShown(value);
  }

  const onLayout = (e) => {
    centerRef.current = { x: e.layout.x + e.layout.width / 2, y: e.layout.y + e.layout.height / 2 };
    if (isPct && Math.abs(e.layout.width - measured) > 0.5) setMeasured(e.layout.width); // resolved px edge
  };

  // Knob geometry at value `k`: the five circles as imperative shape descriptors. The radii/colors are fixed
  // for a given dial size, so the descriptor array is built once (per S) and cached — each move only mutates the
  // shared cx/cy, avoiding five fresh objects + arrays per touch event. (QuickJS allocation + GC in PSRAM is the
  // hot-path cost here.) updateVector serializes the array synchronously, so reusing it across moves is safe.
  const shapesRef = useRef(null);
  const shapesSRef = useRef(-1);
  const knobShapes = (k) => {
    let arr = shapesRef.current;
    if (!arr || shapesSRef.current !== S) {
      arr = KNOB.map((c) => ({ circle: [0, 0, c.r * S], fill: c.fill, stroke: c.stroke, strokeWidth: c.sw * S }));
      shapesRef.current = arr;
      shapesSRef.current = S;
    }
    for (let i = 0; i < arr.length; i++) {
      arr[i].circle[0] = k.x;
      arr[i].circle[1] = k.y;
    }
    return arr;
  };

  // pointer → continuous value: angle from the rendered center, clamp the bottom 90° gap. A drag pushes the
  // knob + readout straight to the engine (no React) and commits to React state only on release.
  const onTouch = (e) => {
    const starting = !draggingRef.current;
    draggingRef.current = true;
    if (starting) {
      prevKRef.current = pointOnArc(angleForValue(lastRef.current, min, max), KNOB_C, KNOB_C, DIAL_R);
      shownIntRef.current = Math.round(lastRef.current);
      statusWordRef.current = statusFor(mode, lastRef.current, current);
    }

    const c = centerRef.current;
    let theta = (Math.atan2(e.x - c.x, -(e.y - c.y)) * 180) / Math.PI; // clockwise-from-top
    theta = clamp(theta, A_START, -A_START); // bottom gap snaps to -135/+135
    const v = clamp(min + ((theta - A_START) / SWEEP) * (max - min), min, max); // continuous, NOT rounded
    if (Math.abs(v - lastRef.current) < 0.08) return; // < ~1px of knob motion: skip
    lastRef.current = v;

    // Imperative update — no setShown, so React never reconciles this subtree mid-drag.
    const k = pointOnArc(angleForValue(v, min, max), KNOB_C, KNOB_C, DIAL_R);
    const pk = prevKRef.current;
    const m = KNOB_R_MAX * S + 3; // repaint margin around the knob (largest circle + AA)
    // Only the old∪new knob box is re-rasterized (the face conic underneath repaints just there, not whole-dial).
    const dirty = [
      Math.min(pk.x, k.x) - m,
      Math.min(pk.y, k.y) - m,
      Math.abs(k.x - pk.x) + 2 * m,
      Math.abs(k.y - pk.y) + 2 * m,
    ];
    updateVector(knobRef.current, knobShapes(k), dirty);
    // Only touch the readout when its string actually changes — otherwise its (centered) box would join the
    // damage rect every move and force a center-to-rim re-raster of the conic face for no visible change.
    const ri = Math.round(v);
    if (ri !== shownIntRef.current) {
      shownIntRef.current = ri;
      updateText(numRef.current, `${ri}°`);
    }
    const sw = statusFor(mode, v, current);
    if (sw !== statusWordRef.current) {
      statusWordRef.current = sw;
      updateText(statusRef.current, sw);
    }
    prevKRef.current = k;
  };

  const onTouchEnd = () => {
    draggingRef.current = false;
    if (lastRef.current !== value) onValue(lastRef.current); // commit the continuous value (no snap)
    setShown(lastRef.current); // re-sync React state so the declarative tree matches the imperative pixels
  };

  // Declarative knob center (for mounts / steppers / the post-release render; the drag path is imperative).
  const k = pointOnArc(angleForValue(shown, min, max), KNOB_C, KNOB_C, DIAL_R);

  return (
    <View
      onLayout={onLayout}
      onTouchStart={onTouch}
      onTouchMove={onTouch}
      onTouchEnd={onTouchEnd}
      style={{ ...boxStyle }}
    >
      {/* Dial face: the imported conic SVG (dark track + cool→warm ghost arc) — a static op-tape, never
          re-uploaded during a drag. */}
      <Svg source={climateFace} style={{ position: 'absolute', left: 0, top: 0, width: box, height: box }} />
      {/* Knob: its own <Svg>, drawn from primitive circles so a drag can move it imperatively (updateVector
          on knobRef) without a React render. The node is oversized by `pad` (and offset by -pad) so the knob
          isn't clipped where the arc nears the dial edge; the same circles render declaratively here for
          mounts / steppers / the post-release re-sync. */}
      <Svg ref={knobRef} style={{ position: 'absolute', left: -pad, top: -pad, width: box + 2 * pad, height: box + 2 * pad }}>
        {KNOB.map((cc, i) => (
          <Circle key={i} cx={k.x} cy={k.y} r={cc.r * S} fill={cc.fill} stroke={cc.stroke} strokeWidth={cc.sw * S} />
        ))}
      </Svg>

      {/* Center readout, absolutely centered over the dial. The status + number are ref'd so the drag path can
          update them imperatively (updateText); "now {current}°" never changes during a drag. */}
      <View
        style={{
          position: 'absolute',
          left: 0,
          top: 0,
          width: box,
          height: box,
          alignItems: 'center',
          justifyContent: 'center',
        }}
      >
        <Text ref={statusRef} style={{ color: theme.subtext, fontSize: sz.sub }}>{statusFor(mode, shown, current)}</Text>
        <Text ref={numRef} style={{ color: theme.text, fontSize: sz.big, fontWeight: '500' }}>{Math.round(shown)}°</Text>
        <Text style={{ color: theme.subtext, fontSize: sz.sub }}>now {current}°</Text>
      </View>
    </View>
  );
}
