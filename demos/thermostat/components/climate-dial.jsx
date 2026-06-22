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
import { View, Text, Svg } from 'embedded-react';
// The dial face + knob are imported SVGs, baked to vector op-tapes by the bundler's .svg loader and drawn
// with <Svg source>. climate-face.svg is the static face (dark track, full-arc ghost gradient, inner
// rings); climate-knob.svg is just the knob, repositioned each render to the value's point on the arc.
import climateFace from '../assets/climate-face.svg';
import climateKnob from '../assets/climate-knob.svg';

// A self-contained dial: it takes its data (value/range), dimensions (size/font sizes), and colors (theme)
// as PROPS, so it imports nothing from the app and can be reused or moved freely. (The app can't expose its
// constants through a shared module here because the Flow B AOT compiler resolves identifiers only within
// the single App.jsx it parses — so app-level constants stay there and reach the dial as props instead.)

// The dial's own intrinsic geometry — the visual arc, independent of the temperature range (which is a prop).
const SWEEP = 270; // the arc spans 270°, with a 90° gap at the bottom
const A_START = -135; // bottom-left, degrees clockwise from 12 o'clock
const DEADBAND = 0.5; // stops the Heating/Cooling status flickering when target ≈ current
// climate-face.svg is cropped tight to the dial: a 410×410 authoring box with the dial centred, the arc at
// radius 185, and (in those same units) the knob art a 72-unit square. SVG_BOX maps that box onto `size`.
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

// ----------------------------------------------------------------------------------------------------
// The arc dial — the rich Flow A (interactive) dial. Face and knob both come from imported SVGs; a drag
// moves the knob (its own <Svg>, translated to the value's point) and updates the center readout. Only the
// wide / stacked layouts render this; the compact (AOT) branch in App draws its own inline arc dial.
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
  const KNOB_BOX = (72 / SVG_BOX) * box; // knob art's box size in screen px

  // Dial center in absolute screen coords, captured from onLayout (a ref, so it never re-renders).
  const centerRef = useRef({ x: 0, y: 0 });
  // The CONTINUOUS value on screen (a float — the knob follows the finger sub-degree). It drives the knob
  // position + readout via state, so a drag re-renders only this subtree (the memoised siblings don't
  // reconcile). The face/knob SVG op-tapes are unchanged across renders, so only the knob node's position
  // updates — no re-raster. Committed back to React state (`value`) on release.
  const [shown, setShown] = useState(value);
  const draggingRef = useRef(false);
  const lastRef = useRef(value); // latest continuous value, tracked synchronously (state batches)
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

  // pointer → continuous value: angle from the rendered center, clamp the bottom 90° gap. Drag advances
  // `shown` (sub-degree, smooth) and commits to React state only on release.
  const onTouch = (e) => {
    draggingRef.current = true;
    const c = centerRef.current;
    let theta = (Math.atan2(e.x - c.x, -(e.y - c.y)) * 180) / Math.PI; // clockwise-from-top
    theta = clamp(theta, A_START, -A_START); // bottom gap snaps to -135/+135
    const v = clamp(min + ((theta - A_START) / SWEEP) * (max - min), min, max); // continuous, NOT rounded
    if (Math.abs(v - lastRef.current) < 0.08) return; // < ~1px of knob motion: skip
    lastRef.current = v;
    setShown(v);
  };

  const onTouchEnd = () => {
    draggingRef.current = false;
    if (lastRef.current !== value) onValue(lastRef.current); // commit the continuous value (no snap)
  };

  // Knob center: the value's point on the SVG dial's arc (authoring radius 185 → DIAL_R on screen).
  const k = pointOnArc(angleForValue(shown, min, max), DIAL_C, DIAL_C, DIAL_R);

  return (
    <View
      onLayout={onLayout}
      onTouchStart={onTouch}
      onTouchMove={onTouch}
      onTouchEnd={onTouchEnd}
      style={{ ...boxStyle }}
    >
      {/* Dial face: the imported SVG (dark track, full-arc ghost gradient, inner rings) — a static op-tape
          baked from climate-face.svg. */}
      <Svg source={climateFace} style={{ position: 'absolute', left: 0, top: 0, width: box, height: box }} />
      {/* Knob: the SVG's knob art, translated to the value's point on the arc. It's radially symmetric, so
          a translate tracks the drag exactly across the whole range (no rotation, no wrap at the ends). */}
      <Svg
        source={climateKnob}
        style={{ position: 'absolute', left: k.x - KNOB_BOX / 2, top: k.y - KNOB_BOX / 2, width: KNOB_BOX, height: KNOB_BOX }}
      />

      {/* Center readout, absolutely centered over the dial. */}
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
        <Text style={{ color: theme.subtext, fontSize: sz.sub }}>{statusFor(mode, shown, current)}</Text>
        <Text style={{ color: theme.text, fontSize: sz.big, fontWeight: '500' }}>{Math.round(shown)}°</Text>
        <Text style={{ color: theme.subtext, fontSize: sz.sub }}>now {current}°</Text>
      </View>
    </View>
  );
}
