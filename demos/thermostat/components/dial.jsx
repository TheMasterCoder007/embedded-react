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
export const START_ANGLE = 150;
export const SWEEP_ANGLE = 240;

// --- Colour mixing -----------------------------------------------------------------------------------
// Used for ONE thing: tinting the centre disc towards the active mode's accent. A View background takes
// no alpha, so the tint is pre-mixed to a solid hex.
const hex3 = color => {
  const packed = parseInt(color.slice(1, 7), 16);
  return [(packed >> 16) & 255, (packed >> 8) & 255, packed & 255];
};
const CHANNEL_MAX = 255;
const clamp255 = channel =>
  channel < 0 ? 0 : channel > CHANNEL_MAX ? CHANNEL_MAX : channel;
export function mix(from, to, amount) {
  const fromChannels = hex3(from);
  const toChannels = hex3(to);
  let hex = '#';
  for (let channelIndex = 0; channelIndex < 3; channelIndex++)
    hex += clamp255(
      Math.round(
        fromChannels[channelIndex] +
          (toChannels[channelIndex] - fromChannels[channelIndex]) * amount,
      ),
    )
      .toString(16)
      .padStart(2, '0');
  return hex;
}

// ----------------------------------------------------------------------------------------------------
// ThermoDial
//
//   geometry   pixel geometry from App ({scale, boxWidth, boxHeight, centerX, centerY, ringRadius, …})
//   theme      resolved palette; accent = the current mode's accent
//   mode       cool | heat | auto | off
//   value      single-setpoint °F (cool / heat / off)
//   low, high  the auto-mode range, °F
//   active     'lo' | 'hi' — which auto handle the steppers move
//   minimum    the bottom of the range the sweep spans
//   maximum    the top of it
//   gap        minimum value kept between the AUTO low and high setpoints (the engine enforces it)
//   fmt        (°F) → display string, e.g. "72°" — owns the unit conversion
//   labels     { ambient } for the centre readout
//   fontSizes  { big, dual, small } font sizes for this layout
//   onCommit   ({value, low, high, which}) once, on release — AUTO commits BOTH ends
//   onPick     ('lo' | 'hi') when a drag grabs an auto handle
// ----------------------------------------------------------------------------------------------------
export function ThermoDial({
  geometry,
  theme,
  accent,
  mode,
  value,
  low,
  high,
  active,
  minimum,
  maximum,
  gap,
  fmt,
  labels,
  fontSizes,
  onCommit,
  onPick,
}) {
  const {
    boxWidth,
    boxHeight,
    centerX,
    centerY,
    ringRadius,
    ringWidth,
    coreRadius,
    scale,
  } = geometry;
  const isAuto = mode === 'auto';
  const isOff = mode === 'off';
  // A <Dial> sizes its ring from its BOX — outer radius is half the smaller side — so the node is a
  // square exactly as wide as the ring's outer edge, centred on the design's (cx, cy).
  const dialSize = 2 * (ringRadius + ringWidth / 2);

  // Live values the drag advances. A drag performs no React render, so this is what the readout and the
  // eventual commit read; it is refreshed from props on every real render (never one mid-drag).
  const liveRef = useRef({value, low, high});
  liveRef.current = {value, low, high};
  const movedRef = useRef(null); // which AUTO end this gesture moved

  const readoutRef = useRef(null);
  const lowRef = useRef(null);
  const highRef = useRef(null);
  const textRef = useRef('');

  // The engine reports every quantized change. Write the new number straight to the text node instead of
  // going through React — on an ESP32-S3 a reconcile costs ~10 ms per node, and a drag produces one of
  // these per frame.
  const onChange = (dialValue, dialLow) => {
    const live = liveRef.current;
    if (isAuto) {
      if (dialLow !== live.low) movedRef.current = 'lo';
      else if (dialValue !== live.high) movedRef.current = 'hi';
      live.low = dialLow;
      live.high = dialValue;
      updateText(lowRef.current, fmt(dialLow));
      updateText(highRef.current, fmt(dialValue));
    } else {
      live.value = dialValue;
      const text = fmt(dialValue);
      if (text !== textRef.current) {
        textRef.current = text;
        updateText(readoutRef.current, text);
      }
    }
  };

  // Commit to React once, on release — the app reconciles a single time per gesture.
  const onRelease = () => {
    const live = liveRef.current;
    if (movedRef.current && movedRef.current !== active)
      onPick(movedRef.current);
    onCommit({
      value: live.value,
      low: live.low,
      high: live.high,
      which: movedRef.current || active,
    });
    movedRef.current = null;
  };

  const dualNum = (which, ref, number) => (
    <Pressable onPress={() => onPick(which)} style={{width: '46%'}}>
      <Text
        ref={ref}
        numberOfLines={1}
        style={{
          fontSize: fontSizes.dual,
          fontWeight: '500',
          color: theme.foreground,
          width: '100%',
          textAlign: which === 'lo' ? 'right' : 'left',
        }}>
        {fmt(number)}
      </Text>
    </Pressable>
  );

  return (
    <View
      style={{
        position: 'absolute',
        left: 0,
        top: 0,
        width: boxWidth,
        height: boxHeight,
      }}>
      <Dial
        style={{
          position: 'absolute',
          left: centerX - dialSize / 2,
          top: centerY - dialSize / 2,
          width: dialSize,
          height: dialSize,
        }}
        min={minimum}
        max={maximum}
        step={0.5}
        startAngle={START_ANGLE}
        sweepAngle={SWEEP_ANGLE}
        thickness={ringWidth}
        trackColor={theme.tick}
        cap="butt"
        range={isAuto}
        valueStart={isAuto ? low : minimum}
        minSpan={gap}
        value={isOff ? minimum : isAuto ? high : value}
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
        knobSize={26 * scale}
        knobColor={theme.foreground}
        adjustable={!isOff}
        onChange={onChange}
        onTouchEnd={onRelease}>
        {/* Face: a single solid accent-tinted disc, sitting at r=coreRadius with clear background out to the
            ring band's inner edge. Its coordinates are relative to the DIAL's box. */}
        <View
          style={{
            position: 'absolute',
            left: dialSize / 2 - coreRadius,
            top: dialSize / 2 - coreRadius,
            width: 2 * coreRadius,
            height: 2 * coreRadius,
            borderRadius: coreRadius,
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
            left: dialSize / 2 - 83 * scale,
            top: dialSize / 2 - 60 * scale,
            width: 166 * scale,
            height: 120 * scale,
            alignItems: 'center',
            justifyContent: 'center',
            gap: 6,
          }}>
          <Text
            style={{
              fontSize: fontSizes.small,
              letterSpacing: 2,
              color: theme.dim,
            }}>
            {labels.ambient}
          </Text>
          {isAuto ? (
            <View
              style={{
                flexDirection: 'row',
                alignItems: 'flex-end',
                gap: 0,
                width: 166 * scale,
              }}>
              {dualNum('lo', lowRef, low)}
              {/* Sized with the numbers, not the captions: as a caption-sized glyph between two readouts
                  three times its height it read as a stray mark rather than a separator. */}
              <Text
                style={{
                  fontSize: fontSizes.dual,
                  color: theme.dim,
                  marginBottom: 1,
                }}>
                -
              </Text>
              {dualNum('hi', highRef, high)}
            </View>
          ) : (
            <Text
              ref={readoutRef}
              numberOfLines={1}
              style={{
                fontSize: fontSizes.big,
                fontWeight: '500',
                color: theme.foreground,
                width: 166 * scale,
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
