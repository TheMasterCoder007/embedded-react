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

import {useState, useRef, useCallback, memo} from 'react';
import {
  View,
  Text,
  Pressable,
  Image,
  Modal,
  Svg,
  Arc,
  Circle,
  Line,
} from 'embedded-react';
import cogIcon from './assets/cog.png';
import {Dial} from './components/dial.jsx';
import {WeatherPanel} from './components/weather.jsx';

// Thermostat — one app, three layouts chosen from the panel size:
//
//   split   ≥760 px wide and landscape (wall tablet 1280×800, dash panel 1024×600, the 800×480 ESP32-S3
//           panel): the arc dial and the 14-day weather panel side by side.
//   stack   tall and at least 330 px wide (a 480×800 phone-shaped panel): the two cards stacked.
//   solo    everything smaller (the 240×320 no-PSRAM CYD): the dial alone.
//
// The dial is a 240° ring carrying one solid arc that fills from the bottom of the range up to the
// setpoint; drag anywhere on the ring to set it. HEAT / COOL / AUTO / OFF each carry their own accent, and
// AUTO lights only the band between a low/high pair, with a handle on each end and a conic warm→cool
// gradient across it. OFF shows the bare track.
//
// Flow A (split / stack) runs the real JS engine and gets the rich <Dial> with an imperative drag. Flow B
// (solo) compiles to C for boards with no JS runtime, so that branch stays inside the AOT subset: hex
// color literals, no cross-module identifiers, and dynamic geometry expressed with <Arc>/<Line> rather
// than a state-driven <Path d>. See the README for the full list of what differs.

// ----------------------------------------------------------------------------------------------------
// Responsive selection — driven by the `screen` global. The Flow A host injects it at runtime; the Flow B
// AOT compiler seeds it from ER_AOT_SCREEN_W/H at build time. Every term below folds to a constant for a
// given panel, so the AOT compiles exactly ONE layout per board and never sees the others.
// ----------------------------------------------------------------------------------------------------
const SW = screen.width;
const SH = screen.height;
const LAYOUT =
  SW >= 760 && SW > SH ? 'split' : SH >= 600 && SW >= 330 ? 'stack' : 'solo';
const SOLO = LAYOUT === 'solo';

const PAD = SOLO ? 8 : 18; // root padding — solo trims it to fund the dial
const GAP = SOLO ? 0 : 16; // gap between the two cards
const CPAD = SOLO ? 10 : 20; // card padding
const CGAP = SOLO ? 8 : 12; // gap between rows inside the thermostat card

const ROOT_W = SW - 2 * PAD;
const ROOT_H = SH - 2 * PAD;
// split: the design gives the thermostat 1.05 of the row against the weather panel's 1.
const THERMO_OUT_W =
  LAYOUT === 'split' ? ((ROOT_W - GAP) * 1.05) / 2.05 : ROOT_W;
const THERMO_OUT_H = LAYOUT === 'stack' ? (ROOT_H - GAP) * 0.52 : ROOT_H;
const WEATHER_OUT_W = LAYOUT === 'split' ? ROOT_W - GAP - THERMO_OUT_W : ROOT_W;
const WEATHER_OUT_H = LAYOUT === 'split' ? ROOT_H : ROOT_H - GAP - THERMO_OUT_H;
const THERMO_W = THERMO_OUT_W - 2 * CPAD;
const THERMO_H = THERMO_OUT_H - 2 * CPAD;
const WEATHER_W = WEATHER_OUT_W - 2 * CPAD;

// The dial box. The design crops its canvas to viewBox "0 34 320 232", which slices ~18 units off the top
// of the r=134 guide ring (and more off the bottom) — on a real panel that just reads as "the dial is cut
// off", so we use the FULL 320×300 canvas instead and let the ring close. The outermost thing drawn is the
// handle at r=135, so 150±135 = 15..285 sits inside 0..300 with a little margin.
//
// Everything below stays inside +-*/ and ternaries: the AOT folds module constants with its own static
// evaluator, which has no Math.min/max/floor — only arithmetic, comparisons, and ?:.
const HEAD_H = SOLO ? 28 : 40; // solo's header is one line (no status), so it needs less
const MODE_H = 44;
// Solo used to give the caption + steppers a row of their own, which cost 44 px of height. Because the
// dial box is locked to the 320:300 authoring aspect, that height was also what capped its WIDTH — the
// card had 51 px of width going unused. Folding the steppers onto the dial's empty bottom (the 240° sweep
// runs 8 o'clock → 4 o'clock, so the bottom of the box is clear) and the caption into the center readout
// buys all of it back: the dial goes from 136 px wide to the full 204, about 50% larger.
const CAPROW_H = 0;

// A LANDSCAPE small panel (a rotated 240x320, or a 480x320) is the opposite problem: height is what is
// scarce and width is what is spare, so stacking the mode buttons UNDER the dial spends the wrong axis —
// at 320x240 it left the dial at 124 px with 160 px of width unused. Landscape therefore puts the modes
// in a COLUMN beside the dial. Both branches fold at compile time, and because only flexDirection and
// these constants differ, one JSX tree serves both.
const LAND = SOLO && SW > SH;
const MODE_W = 56; // landscape: the mode column's width
const ARC_H = LAND
  ? THERMO_H - HEAD_H - CGAP
  : THERMO_H - HEAD_H - MODE_H - CAPROW_H - CGAP * 2;
const ARC_AVAIL_W = LAND ? THERMO_W - MODE_W - CGAP : THERMO_W;
const ARC_MAX = LAYOUT === 'split' ? 400 : LAYOUT === 'stack' ? 360 : 300;
const ARC_FIT_W = (ARC_H * 320) / 300; // widest box whose 320:300 height still fits the arc row
const BOX_CAP = ARC_AVAIL_W < ARC_MAX ? ARC_AVAIL_W : ARC_MAX;
const BOX_FIT = BOX_CAP < ARC_FIT_W ? BOX_CAP : ARC_FIT_W;
const BOX_W = BOX_FIT < 120 ? 120 : BOX_FIT;
const BOX_H = (BOX_W * 300) / 320;
const S = BOX_W / 320; // authoring unit → screen px

const D_CX = 160 * S;
const D_CY = 150 * S;
const D_RRING = 116 * S;
const D_RINGW = 20 * S;
const D_RHAND_IN = 97 * S;
const D_RHAND_OUT = 135 * S;
const D_RCORE = 86 * S; // the solid centre disc — 20 units of clearance to the blocks' 106 inner edge

const GEO = {
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
const SZ = SOLO
  ? {big: 32, dual: 20, small: 10, room: 10, mode: 10}
  : {big: 48, dual: 32, small: 12, room: 12, mode: 12};

// ----------------------------------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------------------------------
const MINF = 50; // the range the 240° sweep spans, °F
const MAXF = 90;
const RGAP = 4; // minimum separation between the AUTO low and high setpoints, °F
const INSIDE_F = 69; // the live room reading (static in this demo)

// Engine angles: 0° at 12 o'clock, growing clockwise. The design's SVG angles (0° at 3 o'clock) are +90.
const A0 = 240;
const SWEEP = 240;
const GRAD_CONIC = 3; // ERGradientType: center (ax,ay) + start angle r, sweeping clockwise from the top

const MODES = [
  // `accent` / `tint` are the DARK palette's values — the solo branch is dark-only and reads them
  // directly, because the AOT resolves `m.accent` on this literal but not a THEME[…][m.key] lookup.
  {key: 'cool', label: 'COOL', accent: '#4FA9F5', tint: '#1a2732'},
  {key: 'heat', label: 'HEAT', accent: '#F2A64B', tint: '#2e261d'},
  {key: 'auto', label: 'AUTO', accent: '#46C4A8', tint: '#192a28'},
  {key: 'off', label: 'OFF', accent: '#8A8F94', tint: '#212426'},
];

// Palettes. Values the design writes as rgba() are pre-mixed to SOLID hex wherever they land on a View
// background or Text color — the engine ignores alpha on a View fill. Alpha survives on borders and on
// vector paints, so `line` / `tick` (both strokes or borders) keep theirs.
const THEMES = {
  dark: {
    bg: '#0a0b0c',
    surface: '#131517',
    fg: '#f2f3f4',
    dim: '#7d7e7f',
    dim2: '#afafb0',
    line: '#ffffff1a',
    tick: '#26282a', // SOLID: see the stroke note in components/dial.jsx
    trackBg: '#26282a',
    barFill: '#969899',
    rainText: '#5B9BD5',
    scrim: '#000000',
    accents: {
      cool: '#4FA9F5',
      heat: '#F2A64B',
      auto: '#46C4A8',
      off: '#8A8F94',
    },
    tints: {cool: '#1a2732', heat: '#2e261d', auto: '#192a28', off: '#212426'},
  },
  light: {
    bg: '#eeeeec',
    surface: '#ffffff',
    fg: '#101112',
    dim: '#8a8a8a',
    dim2: '#525252',
    line: '#0000001c',
    tick: '#ededed', // SOLID: see the stroke note in components/dial.jsx
    trackBg: '#ededed',
    barFill: '#737475',
    rainText: '#2C6698',
    scrim: '#141618',
    accents: {
      cool: '#1D7FD6',
      heat: '#C87C1B',
      auto: '#0E9C7E',
      off: '#767B80',
    },
    tints: {cool: '#edf5fc', heat: '#fbf5ed', auto: '#ecf7f5', off: '#f4f4f5'},
  },
};

// Full-screen centring overlay for the settings sheet
const OVERLAY = {
  position: 'absolute',
  left: 0,
  top: 0,
  width: SW,
  height: SH,
  alignItems: 'center',
  justifyContent: 'center',
};

// Per-theme style objects, built ONCE at module scope. memo(WeatherPanel) only skips re-rendering while
// its props are referentially stable, so the weather card's style cannot be rebuilt each render.
// (Flow A only — the solo branch never references these, so the AOT never has to fold the Math call.)
const CARD = {};
const WEATHER_STYLE = {};
for (const t of ['dark', 'light']) {
  CARD[t] = {
    backgroundColor: THEMES[t].surface,
    borderWidth: 1,
    borderColor: THEMES[t].line,
    borderRadius: 18,
    padding: CPAD,
  };
  WEATHER_STYLE[t] = {
    ...CARD[t],
    width: WEATHER_OUT_W,
    height: WEATHER_OUT_H,
    gap: 14,
  };
}
// The forecast row's flexible column: the card's inner width less the fixed columns and the five 12 px gaps.
const BAR_W = Math.max(24, WEATHER_W - (46 + 24 + 36 + 30 + 30) - 12 * 5);

const clampF = f => (f < MINF ? MINF : f > MAXF ? MAXF : f);

// ----------------------------------------------------------------------------------------------------
// Memoised chrome (Flow A only — the solo branch has its own inline widgets)
//
// A mode switch used to re-render the whole app. On the ESP32-S3 that measured ~405 ms of JavaScript in a
// SINGLE frame: reconciling a node and pushing its props is the dominant cost, so the only lever that
// matters is how many nodes React touches. Everything below takes referentially stable props (module-level
// styles, useState setters, ref-backed callbacks) so a mode or setpoint change skips it entirely.
// ----------------------------------------------------------------------------------------------------
// The sheet's close button. A Pressable is only as big as what it wraps, so hanging it on the bare
// glyph gave a ~16 px target — legible, but smaller than a fingertip. The box is what you aim at, so it
// is sized for touch, and the glyph is just centred in it.
const CLOSE_HIT = {
  width: 40,
  height: 40,
  alignItems: 'center',
  justifyContent: 'center',
};

const STEP_LEFT = {};
const STEP_RIGHT = {};
const SEG = {};
const SEG_TEXT = {};
const SHEET_STYLE = {};
for (const t of ['dark', 'light']) {
  const th = THEMES[t];
  STEP_LEFT[t] = {
    position: 'absolute',
    left: 0,
    bottom: 0,
    width: 52,
    height: 52,
    borderRadius: 26,
    borderWidth: 1,
    borderColor: th.line,
    backgroundColor: th.surface,
    alignItems: 'center',
    justifyContent: 'center',
  };
  STEP_RIGHT[t] = {...STEP_LEFT[t], left: undefined, right: 0};
  const seg = on => ({
    flex: 1,
    height: 44,
    borderRadius: 9,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: on ? th.fg : th.trackBg,
  });
  SEG[t] = {on: seg(true), off: seg(false)};
  SEG_TEXT[t] = {
    on: {fontSize: 12, letterSpacing: 1, color: th.bg},
    off: {fontSize: 12, letterSpacing: 1, color: th.dim},
  };
  SHEET_STYLE[t] = {
    width: 340,
    backgroundColor: th.surface,
    borderWidth: 1,
    borderColor: th.line,
    borderRadius: 16,
    padding: 22,
    gap: 20,
  };
}

const CogButton = memo(function CogButton({onPress}) {
  return (
    <Pressable onPress={onPress}>
      <Image
        source={cogIcon}
        resizeMode="contain"
        style={{width: 30, height: 30}}
      />
    </Pressable>
  );
});

const Stepper = memo(function Stepper({onPress, style, minus, fg}) {
  return (
    <Pressable onPress={onPress} style={style}>
      {minus ? (
        <View style={{width: 16, height: 2, backgroundColor: fg}} />
      ) : (
        <Text style={{fontSize: 20, color: fg}}>+</Text>
      )}
    </Pressable>
  );
});

// Only the two buttons whose `on` actually flips re-render on a mode change.
const ModeButton = memo(function ModeButton({item, on, themeName, onSelect}) {
  const th = THEMES[themeName];
  return (
    <Pressable
      onPress={() => onSelect(item.key)}
      style={{
        flex: 1,
        height: MODE_H,
        borderRadius: 10,
        borderWidth: 1,
        borderColor: on ? th.accents[item.key] : th.line,
        backgroundColor: on ? th.tints[item.key] : th.surface,
        alignItems: 'center',
        justifyContent: 'center',
      }}>
      <Text
        style={{
          fontSize: SZ.mode,
          letterSpacing: 1,
          color: on ? th.accents[item.key] : th.dim,
        }}>
        {item.label}
      </Text>
    </Pressable>
  );
});

// The sheet stays mounted (Modal only toggles its display), so without memo its ~20 nodes reconciled on
// every mode switch even while hidden.
const SettingsSheet = memo(function SettingsSheet({
  visible,
  themeName,
  unit,
  onClose,
  onTheme,
  onUnit,
}) {
  const th = THEMES[themeName];
  const sg = SEG[themeName];
  const st = SEG_TEXT[themeName];
  return (
    <Modal visible={visible} backdropColor={th.scrim} style={OVERLAY}>
      <View style={SHEET_STYLE[themeName]}>
        <View
          style={{
            flexDirection: 'row',
            alignItems: 'center',
            justifyContent: 'space-between',
          }}>
          <Text
            style={{
              fontSize: 12,
              fontWeight: '600',
              letterSpacing: 1,
              color: th.fg,
            }}>
            SETTINGS
          </Text>
          <Pressable onPress={onClose} style={CLOSE_HIT}>
            <Text style={{fontSize: 24, color: th.dim}}>×</Text>
          </Pressable>
        </View>

        <View style={{gap: 9}}>
          <Text style={{fontSize: 10, letterSpacing: 2, color: th.dim}}>
            THEME
          </Text>
          <View style={{flexDirection: 'row', gap: 4}}>
            <Pressable
              onPress={() => onTheme('dark')}
              style={themeName === 'dark' ? sg.on : sg.off}>
              <Text style={themeName === 'dark' ? st.on : st.off}>DARK</Text>
            </Pressable>
            <Pressable
              onPress={() => onTheme('light')}
              style={themeName === 'light' ? sg.on : sg.off}>
              <Text style={themeName === 'light' ? st.on : st.off}>LIGHT</Text>
            </Pressable>
          </View>
        </View>

        <View style={{gap: 9}}>
          <Text style={{fontSize: 10, letterSpacing: 2, color: th.dim}}>
            UNITS
          </Text>
          <View style={{flexDirection: 'row', gap: 4}}>
            <Pressable
              onPress={() => onUnit('F')}
              style={unit === 'F' ? sg.on : sg.off}>
              <Text style={unit === 'F' ? st.on : st.off}>°F</Text>
            </Pressable>
            <Pressable
              onPress={() => onUnit('C')}
              style={unit === 'C' ? sg.on : sg.off}>
              <Text style={unit === 'C' ? st.on : st.off}>°C</Text>
            </Pressable>
          </View>
        </View>
      </View>
    </Modal>
  );
});

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

  // ---- Solo layout (small panels, e.g., the 240×320 CYD) ------------------------------------------
  // Self-contained and inside the Flow B (AOT) subset: a state-driven dial built from <Arc>/<Line> with
  // hex-literal paints, integer steppers, and the mode row as an inlined MODES.map. This is the ONLY
  // branch the AOT compiles on a small board, so nothing below it (the drag dial, the weather panel, the
  // theme switch) is ever reached — which is why those may use the full Flow A feature set.
  //
  // What still differs from Flow A, and why:
  //   - the settings sheet offers UNITS only. A live theme switch would need every color in the tree to
  //     be a ternary of literals, so the palette is baked at build time, and this branch is dark-only.
  //   - the AUTO pair renders as ONE Text ("59°-76°"). The AOT lowers a Text's children to a single
  //     snprintf, so that costs one node where two tappable numbers would cost four, and this branch has
  //     little node headroom (40 of 44 on the CYD).
  //   - which AUTO end the steppers move is set by the last drag rather than by tapping a number.
  //   - the dial is state-driven, not imperative: a move re-renders rather than calling updateVector.
  // What does NOT differ anymore: the arc is a single stroke, and AUTO carries the same conic gradient,
  // both now supported on inline <Svg> shapes in the AOT.
  const cxRef = useRef(0);
  const cyRef = useRef(0);
  // A handler in the AOT subset may only bind `const`s and call a setter, so the angle normalization is a
  // chain of ternaries rather than reassignment. The deadband stops a resistive panel's few pixels of
  // idle noise from wiggling the handle under a held finger.
  const onDrag = useCallback(
    e => {
      const raw =
        (Math.atan2(e.x - cxRef.current, cyRef.current - e.y) * 180) / Math.PI;
      const a0 = raw < 0 ? raw + 360 : raw;
      const a1 = a0 < A0 ? a0 + 360 : a0;
      const a =
        a1 > A0 + SWEEP
          ? a1 - (A0 + SWEEP) < A0 + 360 - a1
            ? A0 + SWEEP
            : A0
          : a1;
      const t = MINF + ((a - A0) / SWEEP) * (MAXF - MINF);
      // AUTO carries a low/high pair instead of one setpoint: grabs whichever end is nearer the finger
      // and drags it, carrying the far end along once they close to RGAP rather than stopping dead. Each
      // end is pre-clamped so it can never be pushed outside the range. Flat `if`s with the whole
      // condition spelled out because an AOT handler may only bind consts and call setters.
      // Which end this gesture owns is decided ONCE, at touch-down (onGrab below), and held in
      // `active` for the rest of the drag. Re-deciding per move would hand the finger over to the other
      // handle the moment it dragged past it, so a handle could never push its neighbor — it would
      // swap instead. The far end is carried along at RGAP rather than stopping dead.
      const isLo = active === 'lo';
      const nLo = t < MINF ? MINF : t > MAXF - RGAP ? MAXF - RGAP : t;
      const nHi = t > MAXF ? MAXF : t < MINF + RGAP ? MINF + RGAP : t;
      if (mode === 'auto' && isLo) setLo(nLo);
      if (mode === 'auto' && isLo && hi < nLo + RGAP) setHi(nLo + RGAP);
      if (mode === 'auto' && !isLo) setHi(nHi);
      if (mode === 'auto' && !isLo && lo > nHi - RGAP) setLo(nHi - RGAP);
      if (mode === 'heat' && Math.abs(t - heatSp) > 0.5) setHeatSp(t);
      // COOL only — OFF draws no arc and no handle, so it must not be draggable either.
      if (mode === 'cool' && Math.abs(t - coolSp) > 0.5) setCoolSp(t);
    },
    [active, mode, hi, lo, heatSp, coolSp],
  );

  // Touch-down: latch whichever AUTO end is nearer the finger, so the drag above knows what it owns.
  const onGrab = useCallback(
    e => {
      const raw =
        (Math.atan2(e.x - cxRef.current, cyRef.current - e.y) * 180) / Math.PI;
      const a0 = raw < 0 ? raw + 360 : raw;
      const a1 = a0 < A0 ? a0 + 360 : a0;
      const a =
        a1 > A0 + SWEEP
          ? a1 - (A0 + SWEEP) < A0 + 360 - a1
            ? A0 + SWEEP
            : A0
          : a1;
      const t = MINF + ((a - A0) / SWEEP) * (MAXF - MINF);
      // Same maths as the drag, but decided from the TOUCH POINT rather than from `active` — this is
      // the one moment the choice is made. It applies the move as well, so a tap on the ring lands the
      // nearer handle there instead of only arming the next move.
      const nearLo = Math.abs(t - lo) <= Math.abs(t - hi);
      const nLo = t < MINF ? MINF : t > MAXF - RGAP ? MAXF - RGAP : t;
      const nHi = t > MAXF ? MAXF : t < MINF + RGAP ? MINF + RGAP : t;
      if (mode === 'auto') setActive(nearLo ? 'lo' : 'hi');
      if (mode === 'auto' && nearLo) setLo(nLo);
      if (mode === 'auto' && nearLo && hi < nLo + RGAP) setHi(nLo + RGAP);
      if (mode === 'auto' && !nearLo) setHi(nHi);
      if (mode === 'auto' && !nearLo && lo > nHi - RGAP) setLo(nHi - RGAP);
      if (mode === 'heat' && Math.abs(t - heatSp) > 0.5) setHeatSp(t);
      if (mode === 'cool' && Math.abs(t - coolSp) > 0.5) setCoolSp(t);
    },
    [value],
  );

  if (SOLO) {
    return (
      <View
        style={{
          width: SW,
          height: SH,
          backgroundColor: '#0a0b0c',
          padding: PAD,
        }}>
        <View
          style={{
            flex: 1,
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
              <View
                style={{
                  position: 'absolute',
                  left: 0,
                  top: 0,
                  width: BOX_W,
                  height: BOX_H,
                }}
                onLayout={e => {
                  cxRef.current = e.layout.x + D_CX;
                  cyRef.current = e.layout.y + D_CY;
                }}
                onTouchStart={onGrab}
                onTouchMove={onDrag}>
                <Svg width={BOX_W} height={BOX_H}>
                  {/* One solid accent-tinted centre disc (pre-mixed surface + accent @ 0.14 — a dynamic colour
                  must be a literal or a ternary of them here). */}
                  <Circle
                    cx={D_CX}
                    cy={D_CY}
                    r={D_RCORE}
                    fill={
                      mode === 'off'
                        ? '#242629'
                        : mode === 'heat'
                          ? '#32291e'
                          : mode === 'auto'
                            ? '#1a2e2b'
                            : '#1b2a36'
                    }
                  />
                  <Arc
                    cx={D_CX}
                    cy={D_CY}
                    r={D_RRING}
                    startAngle={A0}
                    endAngle={A0 + SWEEP}
                    fill="none"
                    stroke="#26282a"
                    strokeWidth={D_RINGW}
                  />
                  <Arc
                    cx={D_CX}
                    cy={D_CY}
                    r={D_RRING}
                    /* AUTO lights only the band BETWEEN its two setpoints, so its arc starts at the low
                   end rather than at the bottom of the range. */
                    startAngle={
                      mode === 'auto'
                        ? A0 + ((lo - MINF) / (MAXF - MINF)) * SWEEP
                        : A0
                    }
                    /* OFF collapses this onto the track's start so only the empty track shows. The setpoint
                   is still held — it is what COOL comes back to — just not drawn. */
                    endAngle={
                      mode === 'off'
                        ? A0
                        : mode === 'auto'
                          ? A0 + ((hi - MINF) / (MAXF - MINF)) * SWEEP
                          : A0 +
                            (((mode === 'heat' ? heatSp : coolSp) - MINF) /
                              (MAXF - MINF)) *
                              SWEEP
                    }
                    fill="none"
                    stroke={
                      mode === 'heat'
                        ? '#F2A64B'
                        : mode === 'auto'
                          ? '#46C4A8'
                          : '#4FA9F5'
                    }
                    /* AUTO ramps warm→cool across the band, the same conic gradient Flow A uses. The
                     sweep starts at the LOW end and `t` runs the full turn from there, so the cool stop
                     sits at span/360, and the engine clamps past it — the ramp therefore always spans
                     exactly lo..hi however wide the band is.
                     CONDITIONAL on purpose: a gradient attached unconditionally still paints in the
                     other modes, where it is anchored to `lo` and so bleeds into COOL/HEAT as their arc
                     grows past that angle. Null outside AUTO leaves the solid `stroke` above. */
                    strokeGrad={
                      mode === 'auto'
                        ? {
                            type: GRAD_CONIC,
                            ax: D_CX,
                            ay: D_CY,
                            r:
                              ((A0 + ((lo - MINF) / (MAXF - MINF)) * SWEEP) *
                                Math.PI) /
                              180,
                            stops: [
                              {color: '#F2A64B', offset: 0},
                              {
                                color: '#4FA9F5',
                                offset:
                                  (((hi - lo) / (MAXF - MINF)) * SWEEP) / 360,
                              },
                            ],
                          }
                        : null
                    }
                    strokeWidth={D_RINGW}
                  />
                  <Line
                    x1={
                      D_CX +
                      D_RHAND_IN *
                        Math.sin(
                          ((A0 +
                            (((mode === 'auto'
                              ? lo
                              : mode === 'heat'
                                ? heatSp
                                : coolSp) -
                              MINF) /
                              (MAXF - MINF)) *
                              SWEEP) *
                            Math.PI) /
                            180,
                        )
                    }
                    y1={
                      D_CY -
                      D_RHAND_IN *
                        Math.cos(
                          ((A0 +
                            (((mode === 'auto'
                              ? lo
                              : mode === 'heat'
                                ? heatSp
                                : coolSp) -
                              MINF) /
                              (MAXF - MINF)) *
                              SWEEP) *
                            Math.PI) /
                            180,
                        )
                    }
                    x2={
                      D_CX +
                      (mode === 'off' ? D_RHAND_IN : D_RHAND_OUT) *
                        Math.sin(
                          ((A0 +
                            (((mode === 'auto'
                              ? lo
                              : mode === 'heat'
                                ? heatSp
                                : coolSp) -
                              MINF) /
                              (MAXF - MINF)) *
                              SWEEP) *
                            Math.PI) /
                            180,
                        )
                    }
                    y2={
                      D_CY -
                      (mode === 'off' ? D_RHAND_IN : D_RHAND_OUT) *
                        Math.cos(
                          ((A0 +
                            (((mode === 'auto'
                              ? lo
                              : mode === 'heat'
                                ? heatSp
                                : coolSp) -
                              MINF) /
                              (MAXF - MINF)) *
                              SWEEP) *
                            Math.PI) /
                            180,
                        )
                    }
                    fill="none"
                    stroke="#f2f3f4"
                    strokeWidth={4 * S}
                  />
                  {/* AUTO's HIGH handle. Zero-length (both ends on the inner radius) in every other mode,
                    the same way OFF collapses the handle above — a collapsed stroke draws nothing. */}
                  <Line
                    x1={
                      D_CX +
                      D_RHAND_IN *
                        Math.sin(
                          ((A0 + ((hi - MINF) / (MAXF - MINF)) * SWEEP) *
                            Math.PI) /
                            180,
                        )
                    }
                    y1={
                      D_CY -
                      D_RHAND_IN *
                        Math.cos(
                          ((A0 + ((hi - MINF) / (MAXF - MINF)) * SWEEP) *
                            Math.PI) /
                            180,
                        )
                    }
                    x2={
                      D_CX +
                      (mode === 'auto' ? D_RHAND_OUT : D_RHAND_IN) *
                        Math.sin(
                          ((A0 + ((hi - MINF) / (MAXF - MINF)) * SWEEP) *
                            Math.PI) /
                            180,
                        )
                    }
                    y2={
                      D_CY -
                      (mode === 'auto' ? D_RHAND_OUT : D_RHAND_IN) *
                        Math.cos(
                          ((A0 + ((hi - MINF) / (MAXF - MINF)) * SWEEP) *
                            Math.PI) /
                            180,
                        )
                    }
                    fill="none"
                    stroke="#f2f3f4"
                    strokeWidth={4 * S}
                  />
                </Svg>

                {/* Centre readout — bounded on BOTH axes, so it never lies across the ring band and swallow
                the touches that drive the drag (its farthest corner is ~102 units out; the band starts
                at 106). A full-height overlay silently kills the drag at the top and bottom of the dial. */}
                <View
                  style={{
                    position: 'absolute',
                    left: D_CX - 83 * S,
                    top: D_CY - 60 * S,
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
                            (((mode === 'heat' ? heatSp : coolSp) - 32) * 5) /
                              9,
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
              </View>

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

        {/* Settings — units only on a small board. The theme is baked at build time. */}
        <Modal visible={settings} backdropColor="#000000" style={OVERLAY}>
          <View
            style={{
              width: SW - 40,
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
                <Text style={{fontSize: 22, color: '#7d7e7f'}}>×</Text>
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

  // ---- Split / stacked layouts (Flow A only) -----------------------------------------------------
  // Below here is the full runtime experience: the imperative drag dial, the <Image>-backed weather
  // panel, and the theme + units sheet. The AOT never reaches it — which is also why everything that
  // needs a real function call or a state-keyed lookup (THEMES[themeName], fmt, step) lives down here
  // rather than at the top of the component, where the AOT would still have to walk it.
  const theme = THEMES[themeName];
  const accent = theme.accents[mode];

  // °F → display string. °C is quantized to half a degree, matching the design.
  const fmt = f => {
    if (unit === 'C') {
      const c = Math.round(((f - 32) * 5) / 9 / 0.5) * 0.5;
      return (Number.isInteger(c) ? c.toFixed(0) : c.toFixed(1)) + '°';
    }
    return Math.round(f) + '°';
  };
  const fmtBare = f =>
    unit === 'C'
      ? String(Math.round(((f - 32) * 5) / 9))
      : String(Math.round(f));

  // A stepper press moves whichever setpoint is live by 1 °F (or half a °C). In AUTO the pair stays
  // RGAP apart by PUSHING the far handle along, matching the drag behavior.
  const step = dir => {
    const d = unit === 'C' ? 0.9 : 1;
    if (mode === 'off') return;
    if (mode === 'auto') {
      if (active === 'lo') {
        const l = clampF(Math.min(lo + dir * d, MAXF - RGAP));
        setLo(l);
        if (hi < l + RGAP) setHi(l + RGAP);
      } else {
        const h = clampF(Math.max(hi + dir * d, MINF + RGAP));
        setHi(h);
        if (lo > h - RGAP) setLo(h - RGAP);
      }
      return;
    }
    const nv = clampF(Math.round(value) + dir * d);
    if (mode === 'heat') setHeatSp(nv);
    else setCoolSp(nv);
  };
  // Ref-backed so these keep the same identity across renders — which is what lets the memoised
  // steppers, cog and mode buttons skip re-rendering.
  const stepRef = useRef(null);
  stepRef.current = step;
  const dec = useCallback(() => stepRef.current(-1), []);
  const inc = useCallback(() => stepRef.current(1), []);
  const openSettings = useCallback(() => setSettings(true), []);
  const closeSettings = useCallback(() => setSettings(false), []);

  // AUTO commits BOTH ends: dragging one can have pushed the other to preserve the gap, and the dial
  // already enforced it live, so these arrive valid.
  const commit = p => {
    if (mode === 'auto') {
      setLo(clampF(p.lo));
      setHi(clampF(p.hi));
      return;
    }
    const v = clampF(p.v);
    if (mode === 'heat') setHeatSp(v);
    else setCoolSp(v);
  };

  // What the equipment is actually doing, from the room reading against the target. It only RUNS while
  // it still has somewhere to go: COOL while the room is above its setpoint, HEAT while it is below,
  // AUTO outside the band either way. Once the target is met nothing is running, so it reads IDLE — the
  // fan stays on AUTO throughout, which is what a real stat shows when it is satisfied.
  const running =
    mode === 'cool'
      ? INSIDE_F > coolSp
      : mode === 'heat'
        ? INSIDE_F < heatSp
        : mode === 'auto'
          ? INSIDE_F > hi || INSIDE_F < lo
          : false;
  const statusLine = running ? 'RUNNING • FAN AUTO' : 'IDLE • FAN AUTO';

  const card = CARD[themeName];

  const thermostat = (
    <View
      style={{...card, width: THERMO_OUT_W, height: THERMO_OUT_H, gap: CGAP}}>
      <View
        style={{
          height: HEAD_H,
          flexDirection: 'row',
          alignItems: 'center',
          justifyContent: 'space-between',
        }}>
        <View style={{gap: 3}}>
          <Text
            style={{
              fontSize: SZ.room,
              fontWeight: '600',
              letterSpacing: 1,
              color: theme.fg,
            }}>
            LIVING ROOM
          </Text>
          <Text style={{fontSize: 10, letterSpacing: 1, color: theme.dim}}>
            {statusLine}
          </Text>
        </View>
        <CogButton onPress={openSettings} />
      </View>

      <View style={{flex: 1, alignItems: 'center', justifyContent: 'center'}}>
        {/* The dial fills this box; the two corner steppers render after it, so they sit on top and take
            their own touches instead of starting a drag. */}
        <View style={{width: BOX_W, height: BOX_H}}>
          <Dial
            geo={GEO}
            theme={theme}
            accent={accent}
            mode={mode}
            value={value}
            lo={lo}
            hi={hi}
            active={active}
            minF={MINF}
            maxF={MAXF}
            gap={RGAP}
            fmt={fmt}
            labels={{ambient: 'INSIDE ' + fmt(INSIDE_F)}}
            sz={SZ}
            onCommit={commit}
            onPick={setActive}
          />
          <Stepper
            onPress={dec}
            style={STEP_LEFT[themeName]}
            minus
            fg={theme.fg}
          />
          <Stepper onPress={inc} style={STEP_RIGHT[themeName]} fg={theme.fg} />
        </View>
      </View>

      <View style={{height: MODE_H, flexDirection: 'row', gap: 6}}>
        {MODES.map(m => (
          <ModeButton
            key={m.key}
            item={m}
            on={mode === m.key}
            themeName={themeName}
            onSelect={setMode}
          />
        ))}
      </View>
    </View>
  );

  // Every prop here is referentially stable across a mode/setpoint change, so memo() skips the whole
  // ~100-node subtree — the single biggest win in the mode-switch cost.
  const weather = (
    <WeatherPanel
      theme={theme}
      unit={unit}
      barW={BAR_W}
      style={WEATHER_STYLE[themeName]}
    />
  );

  return (
    <View
      style={{
        width: SW,
        height: SH,
        backgroundColor: theme.bg,
        padding: PAD,
        flexDirection: LAYOUT === 'split' ? 'row' : 'column',
        gap: GAP,
      }}>
      {thermostat}
      {weather}

      {/* Mounted ONLY while open. A <Modal> left mounted with visible={false} is still a full-screen node
          in the tree. Once a theme change dirties it, its whole-screen rect keeps landing in the
          commit's damage union — so every later interaction repaints all 800×480 (×3 panel buffers)
          instead of just the region that moved. Measured on the ESP32-S3: interaction commit jumped from
          6 to 30 ms to 100–156 ms after switching theme through the sheet, and stayed there even after
          switching back, while flipping the theme WITHOUT opening the sheet never escalated at all. */}
      {settings && (
        <SettingsSheet
          visible={settings}
          themeName={themeName}
          unit={unit}
          onClose={closeSettings}
          onTheme={setTheme}
          onUnit={setUnit}
        />
      )}
    </View>
  );
}
