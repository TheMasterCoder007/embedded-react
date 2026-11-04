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

import {useState, useCallback} from 'react';
import {View, Text, Pressable, Image, Modal, Dial} from 'embedded-react';
import cogIcon from './assets/cog.png';

// Thermostat — one app, three layouts chosen from the panel size:
//
//   split   ≥760 px wide and landscape (wall tablet 1280×800, dash panel 1024×600, the 800×480 ESP32-S3
//           panel): the arc dial and a weather card side by side.
//   stack   tall and at least 330 px wide (a 480×800 phone-shaped panel): the two cards stacked.
//   solo    everything smaller (the 240×320 no-PSRAM CYD): the dial alone.
//
// The dial is a 240° ring carrying one solid arc that fills from the bottom of the range up to the
// setpoint; drag anywhere on the ring to set it. HEAT / COOL / AUTO / OFF each carry their own accent, and
// AUTO lights only the band between a low/high pair, with a handle on each end and a conic warm→cool
// gradient across it. OFF shows the bare track.
//
// This file stays inside the Flow B subset: hex color literals, no cross-module identifiers, and dynamic
// shapes expressed with <Dial>/<Arc> rather than a state-driven <Path d>. One package holds a layout for each
// of the three, and the panel picks at boot. A Flow A build shows RichApp.jsx on the split and stack panels,
// with the imperative drag dial, the 14-day weather panel and the theme switch. See the README for the full
// list of what differs.

// ----------------------------------------------------------------------------------------------------
// Responsive selection — driven by the `screen` global. The Flow A host injects it at runtime; a Flow B
// package works the values below out at boot from the real panel, and holds a layout for each of the three.
// ----------------------------------------------------------------------------------------------------
export const SCREEN_WIDTH = screen.width;
export const SCREEN_HEIGHT = screen.height;
export const LAYOUT =
  SCREEN_WIDTH >= 760 && SCREEN_WIDTH > SCREEN_HEIGHT
    ? 'split'
    : SCREEN_HEIGHT >= 600 && SCREEN_WIDTH >= 330
      ? 'stack'
      : 'solo';
const SOLO = LAYOUT === 'solo';

export const ROOT_PADDING = SOLO ? 8 : 18; // root padding — solo trims it to fund the dial
export const CARD_GAP = SOLO ? 0 : 16; // gap between the two cards
export const CARD_PADDING = SOLO ? 10 : 20; // card padding
export const ROW_GAP = SOLO ? 8 : 12; // gap between rows inside the thermostat card

const ROOT_WIDTH = SCREEN_WIDTH - 2 * ROOT_PADDING;
const ROOT_HEIGHT = SCREEN_HEIGHT - 2 * ROOT_PADDING;
// split: the design gives the thermostat 1.05 of the row against the weather panel's 1.
export const THERMOSTAT_OUTER_WIDTH =
  LAYOUT === 'split' ? ((ROOT_WIDTH - CARD_GAP) * 1.05) / 2.05 : ROOT_WIDTH;
export const THERMOSTAT_OUTER_HEIGHT =
  LAYOUT === 'stack' ? (ROOT_HEIGHT - CARD_GAP) * 0.52 : ROOT_HEIGHT;
export const WEATHER_OUTER_WIDTH =
  LAYOUT === 'split'
    ? ROOT_WIDTH - CARD_GAP - THERMOSTAT_OUTER_WIDTH
    : ROOT_WIDTH;
export const WEATHER_OUTER_HEIGHT =
  LAYOUT === 'split'
    ? ROOT_HEIGHT
    : ROOT_HEIGHT - CARD_GAP - THERMOSTAT_OUTER_HEIGHT;
const THERMOSTAT_WIDTH = THERMOSTAT_OUTER_WIDTH - 2 * CARD_PADDING;
const THERMOSTAT_HEIGHT = THERMOSTAT_OUTER_HEIGHT - 2 * CARD_PADDING;
export const WEATHER_WIDTH = WEATHER_OUTER_WIDTH - 2 * CARD_PADDING;

// The dial box. The design crops its canvas to viewBox "0 34 320 232", which slices ~18 units off the top
// of the r=134 guide ring (and more off the bottom) — on a real panel that just reads as "the dial is cut
// off", so we use the FULL 320×300 canvas instead and let the ring close. The outermost thing drawn is the
// handle at r=135, so 150±135 = 15..285 sits inside 0..300 with a little margin.
const AUTHORING_WIDTH = 320; // the design's dial canvas, in authoring units
const AUTHORING_HEIGHT = 300;
export const HEADER_HEIGHT = SOLO ? 28 : 40; // solo's header is one line (no status), so it needs less
export const MODE_ROW_HEIGHT = 44;
// Solo used to give the caption + steppers a row of their own, which cost 44 px of height. Because the
// dial box is locked to the 320:300 authoring aspect, that height was also what capped its WIDTH — the
// card had 51 px of width going unused. Folding the steppers onto the dial's empty bottom (the 240° sweep
// runs 8 o'clock → 4 o'clock, so the bottom of the box is clear) and the caption into the center readout
// buys all of it back: the dial goes from 136 px wide to the full 204, about 50% larger.
const CAPTION_ROW_HEIGHT = 0;

// A LANDSCAPE small panel (a rotated 240x320, or a 480x320) is the opposite problem: height is what is
// scarce and width is what is spare, so stacking the mode buttons UNDER the dial spends the wrong axis —
// at 320x240 it left the dial at 124 px with 160 px of width unused. Landscape therefore puts the modes
// in a COLUMN beside the dial. Because only flexDirection and these constants differ, one JSX tree
// serves both, and a Flow B build picks between them at boot.
const LANDSCAPE = SOLO && SCREEN_WIDTH > SCREEN_HEIGHT;
const MODE_COLUMN_WIDTH = 56; // landscape: the mode column's width
const ARC_ROW_HEIGHT = LANDSCAPE
  ? THERMOSTAT_HEIGHT - HEADER_HEIGHT - ROW_GAP
  : THERMOSTAT_HEIGHT -
    HEADER_HEIGHT -
    MODE_ROW_HEIGHT -
    CAPTION_ROW_HEIGHT -
    ROW_GAP * 2;
const ARC_AVAILABLE_WIDTH = LANDSCAPE
  ? THERMOSTAT_WIDTH - MODE_COLUMN_WIDTH - ROW_GAP
  : THERMOSTAT_WIDTH;
const ARC_MAX_WIDTH = LAYOUT === 'split' ? 400 : LAYOUT === 'stack' ? 360 : 300;
const ARC_FIT_WIDTH = (ARC_ROW_HEIGHT * AUTHORING_WIDTH) / AUTHORING_HEIGHT; // widest box whose 320:300 height still fits the arc row
export const BOX_WIDTH = Math.max(
  120,
  Math.min(Math.min(ARC_AVAILABLE_WIDTH, ARC_MAX_WIDTH), ARC_FIT_WIDTH),
);
export const BOX_HEIGHT = (BOX_WIDTH * AUTHORING_HEIGHT) / AUTHORING_WIDTH;
const SCALE = BOX_WIDTH / AUTHORING_WIDTH; // authoring unit → screen px

const DIAL_CENTER_X = 160 * SCALE;
const DIAL_CENTER_Y = 150 * SCALE;
const DIAL_RING_RADIUS = 116 * SCALE;
const DIAL_RING_WIDTH = 20 * SCALE;
const DIAL_HANDLE_INNER_RADIUS = 97 * SCALE;
const DIAL_HANDLE_OUTER_RADIUS = 135 * SCALE;
const DIAL_CORE_RADIUS = 86 * SCALE; // the solid center disc — 20 units of clearance to the blocks' 106 inner edge
const DIAL_SIZE = 2 * (116 + 10) * SCALE;

export const DIAL_GEOMETRY = {
  scale: SCALE,
  boxWidth: BOX_WIDTH,
  boxHeight: BOX_HEIGHT,
  centerX: DIAL_CENTER_X,
  centerY: DIAL_CENTER_Y,
  ringRadius: DIAL_RING_RADIUS,
  ringWidth: DIAL_RING_WIDTH,
  coreRadius: DIAL_CORE_RADIUS,
  handleInnerRadius: DIAL_HANDLE_INNER_RADIUS,
  handleOuterRadius: DIAL_HANDLE_OUTER_RADIUS,
};

// Font sizes snap to the engine's baked Inter set (10/12/16/20/24/32/48), so pick from that list.
export const FONT_SIZES = SOLO
  ? {big: 32, dual: 20, small: 10, room: 10, mode: 10}
  : {big: 48, dual: 32, small: 12, room: 12, mode: 12};

// ----------------------------------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------------------------------
export const MIN_FAHRENHEIT = 50; // the range the 240° sweep spans, °F
export const MAX_FAHRENHEIT = 90;
export const RANGE_GAP = 4; // minimum separation between the AUTO low and high setpoints, °F
export const INSIDE_FAHRENHEIT = 69; // the live room reading (static in this demo)

// Engine angles: 0° at 12 o'clock, growing clockwise. The design's SVG angles (0° at 3 o'clock) are +90.
const START_ANGLE = 240;
const SWEEP_ANGLE = 240;
// <Dial> measures its own angles clockwise from 3 o'clock, so the design's START_ANGLE shifts by a quarter turn.
const DIAL_START_ANGLE = START_ANGLE - 90;
const GRADIENT_CONIC = 3; // ERGradientType: center (ax,ay) + start angle r, sweeping clockwise from the top

export const MODES = [
  // `accent` / `tint` are the DARK palette's values — the solo branch is dark-only and reads them
  // directly, because the AOT resolves `item.accent` on this literal but not a THEME[…][item.key] lookup.
  {key: 'cool', label: 'COOL', accent: '#4FA9F5', tint: '#1a2732'},
  {key: 'heat', label: 'HEAT', accent: '#F2A64B', tint: '#2e261d'},
  {key: 'auto', label: 'AUTO', accent: '#46C4A8', tint: '#192a28'},
  {key: 'off', label: 'OFF', accent: '#8A8F94', tint: '#212426'},
];

// Full-screen centring overlay for the settings sheet
export const OVERLAY = {
  position: 'absolute',
  left: 0,
  top: 0,
  width: SCREEN_WIDTH,
  height: SCREEN_HEIGHT,
  alignItems: 'center',
  justifyContent: 'center',
};

// A Flow B panel's weather card: today, then the next few days. Flow A's has the 14-day chart.
const FORECAST = [
  {key: 'tue', day: 'TUE', temps: '74° / 58°'},
  {key: 'wed', day: 'WED', temps: '71° / 55°'},
  {key: 'thu', day: 'THU', temps: '68° / 52°'},
];

// ----------------------------------------------------------------------------------------------------
// App
// ----------------------------------------------------------------------------------------------------
export function App() {
  const [mode, setMode] = useState('cool');
  // COOL and HEAT each remember their own setpoint, so switching back restores what you last set.
  // (AUTO has its own pair below.) OFF mirrors whichever of the two was last active rather than
  // snapping to one of them.
  const [coolSetpoint, setCoolSetpoint] = useState(72);
  const [heatSetpoint, setHeatSetpoint] = useState(68);
  const [low, setLow] = useState(68); // AUTO range
  const [high, setHigh] = useState(76);
  const [active, setActive] = useState('hi'); // which AUTO handle the steppers move

  const [unit, setUnit] = useState('F');
  const [themeName, setTheme] = useState('dark');
  const [settings, setSettings] = useState(false);

  // The live single setpoint for the current mode. OFF mirrors COOL's ring position — its readout is
  // "––" regardless — and AUTO ignores this entirely, using the low/high pair instead. A plain ternary so
  // the AOT can lower it for the solo branch too.
  const value = mode === 'heat' ? heatSetpoint : coolSetpoint;

  // ---- The tree ---------------------------------------------------------------------------------
  // What still differs from Flow A, and why:
  //   - the settings sheet offers UNITS only. A live theme switch would need every color in the tree to
  //     be a ternary of literals, so the palette is baked at build time, and this file is dark-only.
  //   - the AUTO pair renders as ONE Text ("59°-76°"). The AOT lowers a Text's children to a single
  //     formatted string, so that costs one node where two tappable numbers would cost four, and the CYD has
  //     little node headroom (41 of 42).
  //   - which AUTO end the steppers move is set by the last drag rather than by tapping a number.
  //   - the center readout is state-driven, not imperative: a move re-renders that Text rather than
  //     calling updateText. The DRAG itself is native here too — the engine owns it in both flows.
  // What does NOT differ anymore: both flows mount the same native <Dial> node, with the same two-setpoint
  // AUTO band, the same conic gradient, and the same engine-side drag.
  // The dial drives itself: <Dial adjustable> tracks the finger in C (quantized to `step`, latching
  // whichever RANGE end the gesture started nearest and clamping the two so they cannot cross), and
  // reports through onChange. All that is left here is storing what it reports — the ~70 lines of angle
  // normalization, end-latching and per-mode clamping this replaced now live in the engine.
  const onDialChange = useCallback(
    (dialValue, dialLow) => {
      if (mode === 'auto') setLow(dialLow);
      if (mode === 'auto') setHigh(dialValue);
      if (mode === 'heat') setHeatSetpoint(dialValue);
      if (mode === 'cool') setCoolSetpoint(dialValue);
    },
    [mode],
  );

  return (
    <View
      style={{
        width: SCREEN_WIDTH,
        height: SCREEN_HEIGHT,
        backgroundColor: '#0a0b0c',
        padding: ROOT_PADDING,
        flexDirection: LAYOUT === 'split' ? 'row' : 'column',
        gap: CARD_GAP,
      }}>
      <View
        style={{
          width: THERMOSTAT_OUTER_WIDTH,
          height: THERMOSTAT_OUTER_HEIGHT,
          backgroundColor: '#131517',
          borderWidth: 1,
          borderColor: '#ffffff1a',
          borderRadius: 14,
          padding: CARD_PADDING,
          gap: ROW_GAP,
        }}>
        <View
          style={{
            height: HEADER_HEIGHT,
            flexDirection: 'row',
            alignItems: 'center',
            justifyContent: 'space-between',
          }}>
          <Text
            style={{
              fontSize: FONT_SIZES.room,
              fontWeight: '600',
              color: '#f2f3f4',
            }}>
            LIVING ROOM
          </Text>
          <Pressable onPress={() => setSettings(true)}>
            <Image
              source={cogIcon}
              resizeMode="contain"
              style={{width: 24, height: 24}}
            />
          </Pressable>
        </View>

        {/* Dial + modes. Portrait stacks them; landscape sets them side by side so the dial can use
            the height instead of surrendering it to a button row (see LANDSCAPE above). */}
        <View
          style={{
            flex: 1,
            flexDirection: LANDSCAPE ? 'row' : 'column',
            // NO alignItems: children stretch on the cross axis, which is what the mode strip wants
            // in both orientations — full width as a portrait row, full height as a landscape column.
            // The dial box centres itself with alignSelf instead, since it has a fixed size.
            //
            // Landscape spreads them apart so the mode column sits against the card's right edge,
            // the same inset from it as the settings cog (both are bounded by the card padding).
            // Centring the pair instead left the column floating beside the dial with a gap to its
            // right, which read as misaligned against the header.
            justifyContent: LANDSCAPE ? 'space-between' : 'center',
            gap: ROW_GAP,
          }}>
          {/* Dial box. The drag surface is an absolutely-positioned CHILD rather than this wrapper
            itself, so the two steppers can sit on top of it as later siblings and take their own
            touches instead of registering as a drag on the ring — the same arrangement Flow A uses.
            onLayout records the box's centre on screen; a touch anywhere inside sets that angle. */}
          <View
            style={{width: BOX_WIDTH, height: BOX_HEIGHT, alignSelf: 'center'}}>
            <Dial
              style={{
                position: 'absolute',
                left: DIAL_CENTER_X - DIAL_SIZE / 2,
                top: DIAL_CENTER_Y - DIAL_SIZE / 2,
                width: DIAL_SIZE,
                height: DIAL_SIZE,
              }}
              min={MIN_FAHRENHEIT}
              max={MAX_FAHRENHEIT}
              step={0.5}
              startAngle={DIAL_START_ANGLE}
              sweepAngle={SWEEP_ANGLE}
              thickness={DIAL_RING_WIDTH}
              trackColor="#26282a"
              cap="butt"
              range={mode === 'auto'}
              valueStart={mode === 'auto' ? low : MIN_FAHRENHEIT}
              minSpan={RANGE_GAP}
              value={
                mode === 'off'
                  ? MIN_FAHRENHEIT
                  : mode === 'auto'
                    ? high
                    : mode === 'heat'
                      ? heatSetpoint
                      : coolSetpoint
              }
              indicatorColor={
                mode === 'heat'
                  ? '#F2A64B'
                  : mode === 'auto'
                    ? '#46C4A8'
                    : '#4FA9F5'
              }
              indicatorGradient={
                mode === 'auto'
                  ? {
                      type: 'conic',
                      stops: [{color: '#F2A64B'}, {color: '#4FA9F5'}],
                    }
                  : null
              }
              knob={mode === 'off' ? 'none' : 'circle'}
              knobSize={26 * SCALE}
              knobColor="#f2f3f4"
              adjustable={mode !== 'off'}
              onChange={onDialChange}>
              <View
                style={{
                  position: 'absolute',
                  left: DIAL_SIZE / 2 - DIAL_CORE_RADIUS,
                  top: DIAL_SIZE / 2 - DIAL_CORE_RADIUS,
                  width: 2 * DIAL_CORE_RADIUS,
                  height: 2 * DIAL_CORE_RADIUS,
                  borderRadius: DIAL_CORE_RADIUS,
                  backgroundColor:
                    mode === 'off'
                      ? '#242629'
                      : mode === 'heat'
                        ? '#32291e'
                        : mode === 'auto'
                          ? '#1a2e2b'
                          : '#1b2a36',
                }}
              />

              {/* Center readout: a CHILD of the dial (so its coordinates are relative to the dial box).
                Bounded on both axes to sit inside the hole: the engine's ring-only hit test walks up
                from whatever the finger landed on, so content parked in the hole stays inert while the
                surrounding ring still drags — but content that reaches ACROSS the band would take the
                ring's own touches back. Its farthest corner is ~102 units out; the band starts at 106. */}
              <View
                style={{
                  position: 'absolute',
                  left: DIAL_SIZE / 2 - 83 * SCALE,
                  top: DIAL_SIZE / 2 - 60 * SCALE,
                  width: 166 * SCALE,
                  height: 120 * SCALE,
                  alignItems: 'center',
                  justifyContent: 'center',
                  gap: 3,
                }}>
                <Text
                  style={{fontSize: 10, letterSpacing: 2, color: '#7d7e7f'}}>
                  INSIDE{' '}
                  {unit === 'C'
                    ? Math.round(((INSIDE_FAHRENHEIT - 32) * 5) / 9)
                    : INSIDE_FAHRENHEIT}
                  °
                </Text>
                {/* Two nodes rather than one ternary: the AOT lowers a Text child to a single formatted
                string, and it refuses a ternary that mixes the "––" string with the numeric
                branches. */}
                {mode !== 'off' && mode !== 'auto' && (
                  <Text
                    style={{
                      fontSize: FONT_SIZES.big,
                      fontWeight: '500',
                      color: '#f2f3f4',
                    }}>
                    {unit === 'C'
                      ? Math.round(
                          (((mode === 'heat' ? heatSetpoint : coolSetpoint) -
                            32) *
                            5) /
                            9,
                        )
                      : Math.round(
                          mode === 'heat' ? heatSetpoint : coolSetpoint,
                        )}
                    °
                  </Text>
                )}
                {mode === 'off' && (
                  <Text
                    style={{
                      fontSize: FONT_SIZES.big,
                      fontWeight: '500',
                      color: '#f2f3f4',
                    }}>
                    ––
                  </Text>
                )}
                {/* AUTO's pair. ONE Text, not three: the AOT lowers a Text's children to a single
                  formatted string, so "%d° / %d°" costs one node where a colored low/high row would cost
                  four — and this branch has very little node headroom. */}
                {mode === 'auto' && (
                  <Text
                    style={{
                      fontSize: FONT_SIZES.dual,
                      fontWeight: '500',
                      color: '#f2f3f4',
                    }}>
                    {unit === 'C'
                      ? Math.round(((low - 32) * 5) / 9)
                      : Math.round(low)}
                    °-
                    {unit === 'C'
                      ? Math.round(((high - 32) * 5) / 9)
                      : Math.round(high)}
                    °
                  </Text>
                )}
              </View>
            </Dial>

            {/* Step down. Parked in the dial's empty bottom: the sweep runs 8 o'clock to 4 o'clock, so
              nothing is ever drawn below y=139 here and a 44 px target clears the arc's end by 6 px. */}
            <Pressable
              onPressIn={() => {
                // AUTO steps whichever end the last drag grabbed, carrying the other along at RANGE_GAP
                // rather than stopping dead — the same rule the drag uses.
                const lowerLow =
                  Math.round(low) - 1 < MIN_FAHRENHEIT
                    ? MIN_FAHRENHEIT
                    : Math.round(low) - 1;
                const lowerHigh =
                  Math.round(high) - 1 < MIN_FAHRENHEIT + RANGE_GAP
                    ? MIN_FAHRENHEIT + RANGE_GAP
                    : Math.round(high) - 1;
                if (mode === 'auto' && active === 'lo') setLow(lowerLow);
                if (mode === 'auto' && active === 'hi') setHigh(lowerHigh);
                if (
                  mode === 'auto' &&
                  active === 'hi' &&
                  low > lowerHigh - RANGE_GAP
                )
                  setLow(lowerHigh - RANGE_GAP);
                if (mode === 'heat')
                  setHeatSetpoint(setpoint =>
                    Math.round(setpoint) - 1 < MIN_FAHRENHEIT
                      ? MIN_FAHRENHEIT
                      : Math.round(setpoint) - 1,
                  );
                if (mode === 'cool' || mode === 'off')
                  setCoolSetpoint(setpoint =>
                    Math.round(setpoint) - 1 < MIN_FAHRENHEIT
                      ? MIN_FAHRENHEIT
                      : Math.round(setpoint) - 1,
                  );
              }}
              style={{
                position: 'absolute',
                left: 0,
                top: BOX_HEIGHT - 46,
                width: 44,
                height: 44,
                borderRadius: 22,
                borderWidth: 1,
                borderColor: '#ffffff1a',
                alignItems: 'center',
                justifyContent: 'center',
              }}>
              <Text style={{fontSize: 20, color: '#f2f3f4'}}>−</Text>
            </Pressable>

            {/* Step up. Parked in the dial's empty bottom: the sweep runs 8 o'clock to 4 o'clock, so
              nothing is ever drawn below y=139 here and a 44 px target clears the arc's end by 6 px. */}
            <Pressable
              onPressIn={() => {
                const raisedLow =
                  Math.round(low) + 1 > MAX_FAHRENHEIT - RANGE_GAP
                    ? MAX_FAHRENHEIT - RANGE_GAP
                    : Math.round(low) + 1;
                const raisedHigh =
                  Math.round(high) + 1 > MAX_FAHRENHEIT
                    ? MAX_FAHRENHEIT
                    : Math.round(high) + 1;
                if (mode === 'auto' && active === 'lo') setLow(raisedLow);
                if (
                  mode === 'auto' &&
                  active === 'lo' &&
                  high < raisedLow + RANGE_GAP
                )
                  setHigh(raisedLow + RANGE_GAP);
                if (mode === 'auto' && active === 'hi') setHigh(raisedHigh);
                if (mode === 'heat')
                  setHeatSetpoint(setpoint =>
                    Math.round(setpoint) + 1 > MAX_FAHRENHEIT
                      ? MAX_FAHRENHEIT
                      : Math.round(setpoint) + 1,
                  );
                if (mode === 'cool' || mode === 'off')
                  setCoolSetpoint(setpoint =>
                    Math.round(setpoint) + 1 > MAX_FAHRENHEIT
                      ? MAX_FAHRENHEIT
                      : Math.round(setpoint) + 1,
                  );
              }}
              style={{
                position: 'absolute',
                left: BOX_WIDTH - 44,
                top: BOX_HEIGHT - 46,
                width: 44,
                height: 44,
                borderRadius: 22,
                borderWidth: 1,
                borderColor: '#ffffff1a',
                alignItems: 'center',
                justifyContent: 'center',
              }}>
              <Text style={{fontSize: 20, color: '#f2f3f4'}}>+</Text>
            </Pressable>
          </View>

          <View
            style={
              LANDSCAPE
                ? {
                    // An explicit height, not flex: the body centres its children, so a flex column
                    // would size to its content and the buttons inside it would collapse to nothing.
                    width: MODE_COLUMN_WIDTH,
                    height: ARC_ROW_HEIGHT,
                    flexDirection: 'column',
                    gap: 6,
                  }
                : {height: MODE_ROW_HEIGHT, flexDirection: 'row', gap: 6}
            }>
            {MODES.map(item => (
              <Pressable
                key={item.key}
                onPress={() => setMode(item.key)}
                /* No width/height: `flex: 1` sizes the main axis and the default cross-axis stretch
                   fills the other, so ONE style serves the portrait row and the landscape column. A
                   top-level `LAND ? {…} : {…}` would not compile here — the AOT folds a style object
                   statically and these branches read `mode`. */
                style={{
                  flex: 1,
                  borderRadius: 10,
                  borderWidth: 1,
                  borderColor: mode === item.key ? item.accent : '#ffffff1a',
                  backgroundColor: mode === item.key ? item.tint : '#131517',
                  alignItems: 'center',
                  justifyContent: 'center',
                }}>
                <Text
                  style={{
                    fontSize: FONT_SIZES.mode,
                    color: mode === item.key ? item.accent : '#7d7e7f',
                  }}>
                  {item.label}
                </Text>
              </Pressable>
            ))}
          </View>
        </View>
      </View>

      {SOLO ? null : (
        <View
          style={{
            width: WEATHER_OUTER_WIDTH,
            height: WEATHER_OUTER_HEIGHT,
            backgroundColor: '#131517',
            borderWidth: 1,
            borderColor: '#ffffff1a',
            borderRadius: 14,
            padding: CARD_PADDING,
            gap: 12,
          }}>
          <Text style={{fontSize: 12, letterSpacing: 1, color: '#7d7e7f'}}>
            OUTSIDE
          </Text>
          <Text style={{fontSize: 48, color: '#f2f3f4'}}>61°</Text>
          {FORECAST.map(day => (
            <View
              key={day.key}
              style={{flexDirection: 'row', justifyContent: 'space-between'}}>
              <Text style={{fontSize: 16, color: '#7d7e7f'}}>{day.day}</Text>
              <Text style={{fontSize: 16, color: '#f2f3f4'}}>{day.temps}</Text>
            </View>
          ))}
        </View>
      )}

      {/* Settings — units only. The theme is baked at build time. */}
      <Modal visible={settings} backdropColor="#000000" style={OVERLAY}>
        <View
          style={{
            width: Math.min(SCREEN_WIDTH - 40, 340),
            backgroundColor: '#131517',
            borderWidth: 1,
            borderColor: '#ffffff1a',
            borderRadius: 16,
            padding: 16,
            gap: 14,
          }}>
          <View
            style={{
              flexDirection: 'row',
              alignItems: 'center',
              justifyContent: 'space-between',
            }}>
            <Text style={{fontSize: 12, fontWeight: '600', color: '#f2f3f4'}}>
              SETTINGS
            </Text>
            {/* Sized for a fingertip, not for the glyph — see CLOSE_HIT. Inline rather than shared
                with it because this branch compiles ahead of time and takes literals only. */}
            <Pressable
              onPress={() => setSettings(false)}
              style={{
                width: 34,
                height: 34,
                alignItems: 'center',
                justifyContent: 'center',
              }}>
              <Text style={{fontSize: 24, color: '#7d7e7f'}}>×</Text>
            </Pressable>
          </View>
          <Text style={{fontSize: 10, letterSpacing: 2, color: '#7d7e7f'}}>
            UNITS
          </Text>
          <View style={{flexDirection: 'row', gap: 4}}>
            <Pressable
              onPress={() => setUnit('F')}
              style={{
                flex: 1,
                height: 44,
                borderRadius: 9,
                alignItems: 'center',
                justifyContent: 'center',
                backgroundColor: unit === 'F' ? '#f2f3f4' : '#26282a',
              }}>
              <Text
                style={{
                  fontSize: 12,
                  color: unit === 'F' ? '#131517' : '#7d7e7f',
                }}>
                °F
              </Text>
            </Pressable>
            <Pressable
              onPress={() => setUnit('C')}
              style={{
                flex: 1,
                height: 44,
                borderRadius: 9,
                alignItems: 'center',
                justifyContent: 'center',
                backgroundColor: unit === 'C' ? '#f2f3f4' : '#26282a',
              }}>
              <Text
                style={{
                  fontSize: 12,
                  color: unit === 'C' ? '#131517' : '#7d7e7f',
                }}>
                °C
              </Text>
            </Pressable>
          </View>
        </View>
      </Modal>
    </View>
  );
}
