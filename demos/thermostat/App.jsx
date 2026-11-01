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
export const SW = screen.width;
export const SH = screen.height;
export const LAYOUT =
  SW >= 760 && SW > SH ? 'split' : SH >= 600 && SW >= 330 ? 'stack' : 'solo';
const SOLO = LAYOUT === 'solo';

export const PAD = SOLO ? 8 : 18; // root padding — solo trims it to fund the dial
export const GAP = SOLO ? 0 : 16; // gap between the two cards
export const CPAD = SOLO ? 10 : 20; // card padding
export const CGAP = SOLO ? 8 : 12; // gap between rows inside the thermostat card

const ROOT_W = SW - 2 * PAD;
const ROOT_H = SH - 2 * PAD;
// split: the design gives the thermostat 1.05 of the row against the weather panel's 1.
export const THERMO_OUT_W =
  LAYOUT === 'split' ? ((ROOT_W - GAP) * 1.05) / 2.05 : ROOT_W;
export const THERMO_OUT_H = LAYOUT === 'stack' ? (ROOT_H - GAP) * 0.52 : ROOT_H;
export const WEATHER_OUT_W =
  LAYOUT === 'split' ? ROOT_W - GAP - THERMO_OUT_W : ROOT_W;
export const WEATHER_OUT_H =
  LAYOUT === 'split' ? ROOT_H : ROOT_H - GAP - THERMO_OUT_H;
const THERMO_W = THERMO_OUT_W - 2 * CPAD;
const THERMO_H = THERMO_OUT_H - 2 * CPAD;
export const WEATHER_W = WEATHER_OUT_W - 2 * CPAD;

// The dial box. The design crops its canvas to viewBox "0 34 320 232", which slices ~18 units off the top
// of the r=134 guide ring (and more off the bottom) — on a real panel that just reads as "the dial is cut
// off", so we use the FULL 320×300 canvas instead and let the ring close. The outermost thing drawn is the
// handle at r=135, so 150±135 = 15..285 sits inside 0..300 with a little margin.
export const HEAD_H = SOLO ? 28 : 40; // solo's header is one line (no status), so it needs less
export const MODE_H = 44;
// Solo used to give the caption + steppers a row of their own, which cost 44 px of height. Because the
// dial box is locked to the 320:300 authoring aspect, that height was also what capped its WIDTH — the
// card had 51 px of width going unused. Folding the steppers onto the dial's empty bottom (the 240° sweep
// runs 8 o'clock → 4 o'clock, so the bottom of the box is clear) and the caption into the center readout
// buys all of it back: the dial goes from 136 px wide to the full 204, about 50% larger.
const CAPROW_H = 0;

// A LANDSCAPE small panel (a rotated 240x320, or a 480x320) is the opposite problem: height is what is
// scarce and width is what is spare, so stacking the mode buttons UNDER the dial spends the wrong axis —
// at 320x240 it left the dial at 124 px with 160 px of width unused. Landscape therefore puts the modes
// in a COLUMN beside the dial. Because only flexDirection and these constants differ, one JSX tree
// serves both, and a Flow B build picks between them at boot.
const LAND = SOLO && SW > SH;
const MODE_W = 56; // landscape: the mode column's width
const ARC_H = LAND
  ? THERMO_H - HEAD_H - CGAP
  : THERMO_H - HEAD_H - MODE_H - CAPROW_H - CGAP * 2;
const ARC_AVAIL_W = LAND ? THERMO_W - MODE_W - CGAP : THERMO_W;
const ARC_MAX = LAYOUT === 'split' ? 400 : LAYOUT === 'stack' ? 360 : 300;
const ARC_FIT_W = (ARC_H * 320) / 300; // widest box whose 320:300 height still fits the arc row
export const BOX_W = Math.max(
  120,
  Math.min(Math.min(ARC_AVAIL_W, ARC_MAX), ARC_FIT_W),
);
export const BOX_H = (BOX_W * 300) / 320;
const S = BOX_W / 320; // authoring unit → screen px

const D_CX = 160 * S;
const D_CY = 150 * S;
const D_RRING = 116 * S;
const D_RINGW = 20 * S;
const D_RHAND_IN = 97 * S;
const D_RHAND_OUT = 135 * S;
const D_RCORE = 86 * S; // the solid center disc — 20 units of clearance to the blocks' 106 inner edge
const D_DIAL = 2 * (116 + 10) * S;

export const GEO = {
  S,
  boxW: BOX_W,
  boxH: BOX_H,
  cx: D_CX,
  cy: D_CY,
  rRing: D_RRING,
  ringW: D_RINGW,
  rCore: D_RCORE,
  rHandIn: D_RHAND_IN,
  rHandOut: D_RHAND_OUT,
};

// Font sizes snap to the engine's baked Inter set (10/12/16/20/24/32/48), so pick from that list.
export const SZ = SOLO
  ? {big: 32, dual: 20, small: 10, room: 10, mode: 10}
  : {big: 48, dual: 32, small: 12, room: 12, mode: 12};

// ----------------------------------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------------------------------
export const MINF = 50; // the range the 240° sweep spans, °F
export const MAXF = 90;
export const RGAP = 4; // minimum separation between the AUTO low and high setpoints, °F
export const INSIDE_F = 69; // the live room reading (static in this demo)

// Engine angles: 0° at 12 o'clock, growing clockwise. The design's SVG angles (0° at 3 o'clock) are +90.
const A0 = 240;
const SWEEP = 240;
// <Dial> measures its own angles clockwise from 3 o'clock, so the design's A0 shifts by a quarter turn.
const D_A0 = A0 - 90;
const GRAD_CONIC = 3; // ERGradientType: center (ax,ay) + start angle r, sweeping clockwise from the top

export const MODES = [
  // `accent` / `tint` are the DARK palette's values — the solo branch is dark-only and reads them
  // directly, because the AOT resolves `m.accent` on this literal but not a THEME[…][m.key] lookup.
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
  width: SW,
  height: SH,
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
  const [coolSp, setCoolSp] = useState(72);
  const [heatSp, setHeatSp] = useState(68);
  const [lo, setLo] = useState(68); // AUTO range
  const [hi, setHi] = useState(76);
  const [active, setActive] = useState('hi'); // which AUTO handle the steppers move

  const [unit, setUnit] = useState('F');
  const [themeName, setTheme] = useState('dark');
  const [settings, setSettings] = useState(false);

  // The live single setpoint for the current mode. OFF mirrors COOL's ring position — its readout is
  // "––" regardless — and AUTO ignores this entirely, using the lo/hi pair instead. A plain ternary so
  // the AOT can lower it for the solo branch too.
  const value = mode === 'heat' ? heatSp : coolSp;

  // ---- The tree ---------------------------------------------------------------------------------
  // What still differs from Flow A, and why:
  //   - the settings sheet offers UNITS only. A live theme switch would need every color in the tree to
  //     be a ternary of literals, so the palette is baked at build time, and this file is dark-only.
  //   - the AUTO pair renders as ONE Text ("59°-76°"). The AOT lowers a Text's children to a single
  //     snprintf, so that costs one node where two tappable numbers would cost four, and the CYD has
  //     little node headroom (40 of 44).
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
    (v, vLo) => {
      if (mode === 'auto') setLo(vLo);
      if (mode === 'auto') setHi(v);
      if (mode === 'heat') setHeatSp(v);
      if (mode === 'cool') setCoolSp(v);
    },
    [mode],
  );

  return (
    <View
      style={{
        width: SW,
        height: SH,
        backgroundColor: '#0a0b0c',
        padding: PAD,
        flexDirection: LAYOUT === 'split' ? 'row' : 'column',
        gap: GAP,
      }}>
      <View
        style={{
          width: THERMO_OUT_W,
          height: THERMO_OUT_H,
          backgroundColor: '#131517',
          borderWidth: 1,
          borderColor: '#ffffff1a',
          borderRadius: 14,
          padding: CPAD,
          gap: CGAP,
        }}>
        <View
          style={{
            height: HEAD_H,
            flexDirection: 'row',
            alignItems: 'center',
            justifyContent: 'space-between',
          }}>
          <Text
            style={{fontSize: SZ.room, fontWeight: '600', color: '#f2f3f4'}}>
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
            the height instead of surrendering it to a button row (see LAND above). */}
        <View
          style={{
            flex: 1,
            flexDirection: LAND ? 'row' : 'column',
            // NO alignItems: children stretch on the cross axis, which is what the mode strip wants
            // in both orientations — full width as a portrait row, full height as a landscape column.
            // The dial box centres itself with alignSelf instead, since it has a fixed size.
            //
            // Landscape spreads them apart so the mode column sits against the card's right edge,
            // the same inset from it as the settings cog (both are bounded by the card padding).
            // Centring the pair instead left the column floating beside the dial with a gap to its
            // right, which read as misaligned against the header.
            justifyContent: LAND ? 'space-between' : 'center',
            gap: CGAP,
          }}>
          {/* Dial box. The drag surface is an absolutely-positioned CHILD rather than this wrapper
            itself, so the two steppers can sit on top of it as later siblings and take their own
            touches instead of registering as a drag on the ring — the same arrangement Flow A uses.
            onLayout records the box's centre on screen; a touch anywhere inside sets that angle. */}
          <View style={{width: BOX_W, height: BOX_H, alignSelf: 'center'}}>
            <Dial
              style={{
                position: 'absolute',
                left: D_CX - D_DIAL / 2,
                top: D_CY - D_DIAL / 2,
                width: D_DIAL,
                height: D_DIAL,
              }}
              min={MINF}
              max={MAXF}
              step={0.5}
              startAngle={D_A0}
              sweepAngle={SWEEP}
              thickness={D_RINGW}
              trackColor="#26282a"
              cap="butt"
              range={mode === 'auto'}
              valueStart={mode === 'auto' ? lo : MINF}
              minSpan={RGAP}
              value={
                mode === 'off'
                  ? MINF
                  : mode === 'auto'
                    ? hi
                    : mode === 'heat'
                      ? heatSp
                      : coolSp
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
              knobSize={26 * S}
              knobColor="#f2f3f4"
              adjustable={mode !== 'off'}
              onChange={onDialChange}>
              <View
                style={{
                  position: 'absolute',
                  left: D_DIAL / 2 - D_RCORE,
                  top: D_DIAL / 2 - D_RCORE,
                  width: 2 * D_RCORE,
                  height: 2 * D_RCORE,
                  borderRadius: D_RCORE,
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
                  left: D_DIAL / 2 - 83 * S,
                  top: D_DIAL / 2 - 60 * S,
                  width: 166 * S,
                  height: 120 * S,
                  alignItems: 'center',
                  justifyContent: 'center',
                  gap: 3,
                }}>
                <Text
                  style={{fontSize: 10, letterSpacing: 2, color: '#7d7e7f'}}>
                  INSIDE{' '}
                  {unit === 'C'
                    ? Math.round(((INSIDE_F - 32) * 5) / 9)
                    : INSIDE_F}
                  °
                </Text>
                {/* Two nodes rather than one ternary: the AOT lowers a Text child to a single snprintf, and
                mixing the "––" string with the numeric branches emits a pointer-vs-int conditional that
                a C compiler rejects. */}
                {mode !== 'off' && mode !== 'auto' && (
                  <Text
                    style={{
                      fontSize: SZ.big,
                      fontWeight: '500',
                      color: '#f2f3f4',
                    }}>
                    {unit === 'C'
                      ? Math.round(
                          (((mode === 'heat' ? heatSp : coolSp) - 32) * 5) / 9,
                        )
                      : Math.round(mode === 'heat' ? heatSp : coolSp)}
                    °
                  </Text>
                )}
                {mode === 'off' && (
                  <Text
                    style={{
                      fontSize: SZ.big,
                      fontWeight: '500',
                      color: '#f2f3f4',
                    }}>
                    ––
                  </Text>
                )}
                {/* AUTO's pair. ONE Text, not three: the AOT lowers a Text's children to a single
                  snprintf, so "%d° / %d°" costs one node where a colored low/high row would cost
                  four — and this branch has very little node headroom. */}
                {mode === 'auto' && (
                  <Text
                    style={{
                      fontSize: SZ.dual,
                      fontWeight: '500',
                      color: '#f2f3f4',
                    }}>
                    {unit === 'C'
                      ? Math.round(((lo - 32) * 5) / 9)
                      : Math.round(lo)}
                    °-
                    {unit === 'C'
                      ? Math.round(((hi - 32) * 5) / 9)
                      : Math.round(hi)}
                    °
                  </Text>
                )}
              </View>
            </Dial>

            {/* Step down. Parked in the dial's empty bottom: the sweep runs 8 o'clock to 4 o'clock, so
              nothing is ever drawn below y=139 here and a 44 px target clears the arc's end by 6 px. */}
            <Pressable
              onPressIn={() => {
                // AUTO steps whichever end the last drag grabbed, carrying the other along at RGAP
                // rather than stopping dead — the same rule the drag uses.
                const dLo =
                  Math.round(lo) - 1 < MINF ? MINF : Math.round(lo) - 1;
                const dHi =
                  Math.round(hi) - 1 < MINF + RGAP
                    ? MINF + RGAP
                    : Math.round(hi) - 1;
                if (mode === 'auto' && active === 'lo') setLo(dLo);
                if (mode === 'auto' && active === 'hi') setHi(dHi);
                if (mode === 'auto' && active === 'hi' && lo > dHi - RGAP)
                  setLo(dHi - RGAP);
                if (mode === 'heat')
                  setHeatSp(v =>
                    Math.round(v) - 1 < MINF ? MINF : Math.round(v) - 1,
                  );
                if (mode === 'cool' || mode === 'off')
                  setCoolSp(v =>
                    Math.round(v) - 1 < MINF ? MINF : Math.round(v) - 1,
                  );
              }}
              style={{
                position: 'absolute',
                left: 0,
                top: BOX_H - 46,
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
                const uLo =
                  Math.round(lo) + 1 > MAXF - RGAP
                    ? MAXF - RGAP
                    : Math.round(lo) + 1;
                const uHi =
                  Math.round(hi) + 1 > MAXF ? MAXF : Math.round(hi) + 1;
                if (mode === 'auto' && active === 'lo') setLo(uLo);
                if (mode === 'auto' && active === 'lo' && hi < uLo + RGAP)
                  setHi(uLo + RGAP);
                if (mode === 'auto' && active === 'hi') setHi(uHi);
                if (mode === 'heat')
                  setHeatSp(v =>
                    Math.round(v) + 1 > MAXF ? MAXF : Math.round(v) + 1,
                  );
                if (mode === 'cool' || mode === 'off')
                  setCoolSp(v =>
                    Math.round(v) + 1 > MAXF ? MAXF : Math.round(v) + 1,
                  );
              }}
              style={{
                position: 'absolute',
                left: BOX_W - 44,
                top: BOX_H - 46,
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
              LAND
                ? {
                    // An explicit height, not flex: the body centres its children, so a flex column
                    // would size to its content and the buttons inside it would collapse to nothing.
                    width: MODE_W,
                    height: ARC_H,
                    flexDirection: 'column',
                    gap: 6,
                  }
                : {height: MODE_H, flexDirection: 'row', gap: 6}
            }>
            {MODES.map(m => (
              <Pressable
                key={m.key}
                onPress={() => setMode(m.key)}
                /* No width/height: `flex: 1` sizes the main axis and the default cross-axis stretch
                   fills the other, so ONE style serves the portrait row and the landscape column. A
                   top-level `LAND ? {…} : {…}` would not compile here — the AOT folds a style object
                   statically and these branches read `mode`. */
                style={{
                  flex: 1,
                  borderRadius: 10,
                  borderWidth: 1,
                  borderColor: mode === m.key ? m.accent : '#ffffff1a',
                  backgroundColor: mode === m.key ? m.tint : '#131517',
                  alignItems: 'center',
                  justifyContent: 'center',
                }}>
                <Text
                  style={{
                    fontSize: SZ.mode,
                    color: mode === m.key ? m.accent : '#7d7e7f',
                  }}>
                  {m.label}
                </Text>
              </Pressable>
            ))}
          </View>
        </View>
      </View>

      {SOLO ? null : (
        <View
          style={{
            width: WEATHER_OUT_W,
            height: WEATHER_OUT_H,
            backgroundColor: '#131517',
            borderWidth: 1,
            borderColor: '#ffffff1a',
            borderRadius: 14,
            padding: CPAD,
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
            width: Math.min(SW - 40, 340),
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
