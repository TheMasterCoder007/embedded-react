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

import {useRef} from 'react';
import {View, Text, Pressable, Dial, updateText} from 'embedded-react';

// The thermostat dial — a 240° ring whose band fills from the bottom of the range up to the setpoint,
// with a knob riding its leading edge. AUTO lights only the band BETWEEN its two setpoints and puts a
// knob on each end.
//
// This is now one native <Dial> node (ER_NODE_ARC). It replaced ~450 lines that built two <Svg> op-tapes
// by hand and repainted them per touch sample through updateVector, plus the swept-damage-rect maths that
// kept those repaints cheap — the engine does all of it now, and does it better: a value change repaints
// only the sliver the band swept plus the knob's two spots, and the drag itself never enters JS.
//
// What is still done imperatively here is the CENTRE READOUT. The drag runs entirely in C, so a move
// costs no React work at all unless we ask for some — updateText writes the new number straight to the
// text node, and React state is committed once, on release. That is the whole reason this component
// still holds refs.
//
// It is self-contained — geometry, palette and values all arrive as props — so it imports nothing from
// the app. (The compact `solo` layout in App.jsx inlines its own <Dial>: the Flow B AOT compiler resolves
// identifiers only inside the single App.jsx it parses, so it cannot reach across this module.)

// Angles. The design's sweep is SVG degrees (0° at 3 o'clock): 150° → 390°, a 240° arc with a 120° dead
// zone centred on the bottom. <Dial> uses that same convention, so they carry straight over.
export const A0 = 150;
export const SWEEP = 240;

// --- Colour mixing -----------------------------------------------------------------------------------
// Used for ONE thing: tinting the centre disc towards the active mode's accent. A View background takes
// no alpha, so the tint is pre-mixed to a solid hex.
const hex3 = c => {
  const n = parseInt(c.slice(1, 7), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
};
const clamp255 = v => (v < 0 ? 0 : v > 255 ? 255 : v);
export function mix(a, b, t) {
  const A = hex3(a);
  const B = hex3(b);
  let s = '#';
  for (let i = 0; i < 3; i++)
    s += clamp255(Math.round(A[i] + (B[i] - A[i]) * t))
      .toString(16)
      .padStart(2, '0');
  return s;
}

// ----------------------------------------------------------------------------------------------------
// ThermoDial
//
//   geo       pixel geometry from App ({S, boxW, boxH, cx, cy, rRing, ringW, rCore, …})
//   theme     resolved palette; accent = the current mode's accent
//   mode      cool | heat | auto | off
//   value     single-setpoint °F (cool / heat / off)
//   lo, hi    the auto-mode range, °F
//   active    'lo' | 'hi' — which auto handle the steppers move
//   minF/maxF the range the sweep spans
//   gap       minimum value kept between the AUTO low and high setpoints (the engine enforces it)
//   fmt       (°F) → display string, e.g. "72°" — owns the unit conversion
//   labels    { ambient } for the centre readout
//   sz        { big, dual, small } font sizes for this layout
//   onCommit  ({v, lo, hi, which}) once, on release — AUTO commits BOTH ends
//   onPick    ('lo' | 'hi') when a drag grabs an auto handle
// ----------------------------------------------------------------------------------------------------
export function ThermoDial({
  geo,
  theme,
  accent,
  mode,
  value,
  lo,
  hi,
  active,
  minF,
  maxF,
  gap,
  fmt,
  labels,
  sz,
  onCommit,
  onPick,
}) {
  const {boxW, boxH, cx, cy, rRing, ringW, rCore, S} = geo;
  const isAuto = mode === 'auto';
  const isOff = mode === 'off';
  // A <Dial> sizes its ring from its BOX — outer radius is half the smaller side — so the node is a
  // square exactly as wide as the ring's outer edge, centred on the design's (cx, cy).
  const dialSize = 2 * (rRing + ringW / 2);

  // Live values the drag advances. A drag performs no React render, so this is what the readout and the
  // eventual commit read; it is refreshed from props on every real render (never one mid-drag).
  const liveRef = useRef({v: value, lo, hi});
  liveRef.current = {v: value, lo, hi};
  const movedRef = useRef(null); // which AUTO end this gesture moved

  const bigRef = useRef(null);
  const loRef = useRef(null);
  const hiRef = useRef(null);
  const textRef = useRef('');

  // The engine reports every quantized change. Write the new number straight to the text node instead of
  // going through React — on an ESP32-S3 a reconcile costs ~10 ms per node, and a drag produces one of
  // these per frame.
  const onChange = (v, vLo) => {
    const live = liveRef.current;
    if (isAuto) {
      if (vLo !== live.lo) movedRef.current = 'lo';
      else if (v !== live.hi) movedRef.current = 'hi';
      live.lo = vLo;
      live.hi = v;
      updateText(loRef.current, fmt(vLo));
      updateText(hiRef.current, fmt(v));
    } else {
      live.v = v;
      const s = fmt(v);
      if (s !== textRef.current) {
        textRef.current = s;
        updateText(bigRef.current, s);
      }
    }
  };

  // Commit to React once, on release — the app reconciles a single time per gesture.
  const onRelease = () => {
    const live = liveRef.current;
    if (movedRef.current && movedRef.current !== active)
      onPick(movedRef.current);
    onCommit({
      v: live.v,
      lo: live.lo,
      hi: live.hi,
      which: movedRef.current || active,
    });
    movedRef.current = null;
  };

  const dualNum = (which, ref, v) => (
    <Pressable onPress={() => onPick(which)} style={{width: '46%'}}>
      <Text
        ref={ref}
        numberOfLines={1}
        style={{
          fontSize: sz.dual,
          fontWeight: '500',
          color: theme.fg,
          width: '100%',
          textAlign: which === 'lo' ? 'right' : 'left',
        }}>
        {fmt(v)}
      </Text>
    </Pressable>
  );

  return (
    <View
      style={{
        position: 'absolute',
        left: 0,
        top: 0,
        width: boxW,
        height: boxH,
      }}>
      <Dial
        style={{
          position: 'absolute',
          left: cx - dialSize / 2,
          top: cy - dialSize / 2,
          width: dialSize,
          height: dialSize,
        }}
        min={minF}
        max={maxF}
        step={0.5}
        startAngle={A0}
        sweepAngle={SWEEP}
        thickness={ringW}
        trackColor={theme.tick}
        cap="butt"
        range={isAuto}
        valueStart={isAuto ? lo : minF}
        minSpan={gap}
        value={isOff ? minF : isAuto ? hi : value}
        indicatorColor={accent}
        indicatorGradient={
          isAuto
            ? {
                type: 'conic',
                stops: [
                  {color: theme.accents.heat},
                  {color: theme.accents.cool},
                ],
              }
            : null
        }
        knob={isOff ? 'none' : 'circle'}
        knobSize={26 * S}
        knobColor={theme.fg}
        adjustable={!isOff}
        onChange={onChange}
        onTouchEnd={onRelease}>
        {/* Face: a single solid accent-tinted disc, sitting at r=rCore with clear background out to the
            ring band's inner edge. Its coordinates are relative to the DIAL's box. */}
        <View
          style={{
            position: 'absolute',
            left: dialSize / 2 - rCore,
            top: dialSize / 2 - rCore,
            width: 2 * rCore,
            height: 2 * rCore,
            borderRadius: rCore,
            backgroundColor: mix(theme.surface, accent, 0.14),
          }}
        />

        {/* Center readout. Deliberately BOUNDED on both axes so it stays inside the hole: the engine's
            ring-only hit test walks up from whatever the finger landed on, so content parked in the hole
            is inert while the surrounding ring still drags — but content reaching ACROSS the band would
            take the ring's own touches back. At ±83 × ±60 authoring units its farthest corner is ~102
            units out, just inside the band's 106 inner edge. */}
        <View
          style={{
            position: 'absolute',
            left: dialSize / 2 - 83 * S,
            top: dialSize / 2 - 60 * S,
            width: 166 * S,
            height: 120 * S,
            alignItems: 'center',
            justifyContent: 'center',
            gap: 6,
          }}>
          <Text
            style={{fontSize: sz.small, letterSpacing: 2, color: theme.dim}}>
            {labels.ambient}
          </Text>
          {isAuto ? (
            <View
              style={{
                flexDirection: 'row',
                alignItems: 'flex-end',
                gap: 0,
                width: 166 * S,
              }}>
              {dualNum('lo', loRef, lo)}
              {/* Sized with the numbers, not the captions: as a caption-sized glyph between two readouts
                  three times its height it read as a stray mark rather than a separator. */}
              <Text
                style={{fontSize: sz.dual, color: theme.dim, marginBottom: 1}}>
                -
              </Text>
              {dualNum('hi', hiRef, hi)}
            </View>
          ) : (
            <Text
              ref={bigRef}
              numberOfLines={1}
              style={{
                fontSize: sz.big,
                fontWeight: '500',
                color: theme.fg,
                width: 166 * S,
                textAlign: 'center',
              }}>
              {isOff ? '––' : fmt(value)}
            </Text>
          )}
        </View>
      </Dial>
    </View>
  );
}
