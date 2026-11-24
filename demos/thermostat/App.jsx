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
import {
  ARC_ROW_HEIGHT,
  BOX_HEIGHT,
  BOX_WIDTH,
  CARD_GAP,
  CARD_PADDING,
  DIAL_CENTER_X,
  DIAL_CENTER_Y,
  DIAL_CORE_RADIUS,
  DIAL_RING_WIDTH,
  DIAL_SIZE,
  DIAL_START_ANGLE,
  FONT_SIZES,
  FORECAST,
  HEADER_HEIGHT,
  INSIDE_FAHRENHEIT,
  LANDSCAPE,
  LAYOUT,
  MAX_FAHRENHEIT,
  MIN_FAHRENHEIT,
  MODES,
  MODE_COLUMN_WIDTH,
  MODE_ROW_HEIGHT,
  OVERLAY,
  RANGE_GAP,
  ROOT_PADDING,
  ROW_GAP,
  SCALE,
  SCREEN_HEIGHT,
  SCREEN_WIDTH,
  SOLO,
  SWEEP_ANGLE,
  THERMOSTAT_OUTER_HEIGHT,
  THERMOSTAT_OUTER_WIDTH,
  WEATHER_OUTER_HEIGHT,
  WEATHER_OUTER_WIDTH,
} from './layout.js';

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
// This file and layout.js, which holds the sizes and the model, stay inside the Flow B subset: hex color
// literals, and dynamic shapes expressed with <Dial>/<Arc> rather than a state-driven <Path d>. The Flow B
// compiler follows the import and compiles the two as one app. One package holds a layout for each
// of the three, and the panel picks at boot. A Flow A build shows RichApp.jsx on the split and stack panels,
// with the imperative drag dial, the 14-day weather panel and the theme switch. See the README for the full
// list of what differs.

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
                {/* One Text per mode, in a ternary rather than a Text child: the AOT lowers a Text's children
                to a single formatted string and refuses a ternary that mixes "––" with numbers. Only
                the picked branch is built, so the three cost one node. */}
                {mode === 'off' ? (
                  <Text
                    style={{
                      fontSize: FONT_SIZES.big,
                      fontWeight: '500',
                      color: '#f2f3f4',
                    }}>
                    ––
                  </Text>
                ) : mode === 'auto' ? (
                  /* AUTO's pair. ONE Text, not three: "%d° / %d°" costs one node where a colored
                    low/high row would cost four. */
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
                ) : (
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
