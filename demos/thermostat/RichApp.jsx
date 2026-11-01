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
import {View, Text, Pressable, Image, Modal} from 'embedded-react';
import cogIcon from './assets/cog.png';
import {ThermoDial} from './components/dial.jsx';
import {WeatherPanel} from './components/weather.jsx';
import {HeaderClock, ClockSetter} from './components/clock.jsx';
import {
  HAS_NETWORK,
  NetworkRows,
  WifiPage,
  ZonePage,
} from './components/network.jsx';
import {
  BOX_H,
  BOX_W,
  CGAP,
  CPAD,
  GAP,
  GEO,
  HEAD_H,
  INSIDE_F,
  LAYOUT,
  MAXF,
  MINF,
  MODES,
  MODE_H,
  OVERLAY,
  PAD,
  RGAP,
  SH,
  SW,
  SZ,
  THERMO_OUT_H,
  THERMO_OUT_W,
  WEATHER_OUT_H,
  WEATHER_OUT_W,
  WEATHER_W,
} from './App.jsx';

// The thermostat's split and stack layouts in Flow A, on the real JS engine: the imperative drag dial, the
// <Image>-backed weather panel, and the theme + units sheet. Flow B doesn't compile this file: App.jsx is its
// whole app. index.jsx picks between the two.

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

// Per-theme style objects, built ONCE at module scope. memo(WeatherPanel) only skips re-rendering while
// its props are referentially stable, so the weather card's style cannot be rebuilt each render.
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
// Memoised chrome
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
const SHEET_WIDE_STYLE = {}; // with the network column beside the rest
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
  SHEET_WIDE_STYLE[t] = {...SHEET_STYLE[t], width: 664};
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

// The sheet's body: one column, or with network settings a second one beside it, since the sheet is too
// short for both stacked on an 800x480 panel.
function Columns({wide, side, children}) {
  if (!wide) return children;
  return (
    <View style={{flexDirection: 'row', gap: 28}}>
      <View style={{width: 296, gap: 20}}>{children}</View>
      <View style={{width: 296}}>{side}</View>
    </View>
  );
}

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
  // The Wi-Fi and time-zone pages take the sheet's place while one is open.
  const [page, setPage] = useState(null);
  const back = useCallback(() => setPage(null), []);
  if (page) {
    return (
      <Modal visible={visible} backdropColor={th.scrim} style={OVERLAY}>
        {page === 'wifi' ? (
          <WifiPage theme={th} onDone={back} />
        ) : (
          <ZonePage theme={th} onDone={back} />
        )}
      </Modal>
    );
  }
  return (
    <Modal visible={visible} backdropColor={th.scrim} style={OVERLAY}>
      <View
        style={
          HAS_NETWORK ? SHEET_WIDE_STYLE[themeName] : SHEET_STYLE[themeName]
        }>
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

        <Columns
          wide={HAS_NETWORK}
          side={<NetworkRows theme={th} onOpen={setPage} />}>
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
                <Text style={themeName === 'light' ? st.on : st.off}>
                  LIGHT
                </Text>
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

          <ClockSetter theme={th} />
        </Columns>
      </View>
    </Modal>
  );
});

// ----------------------------------------------------------------------------------------------------
// App
// ----------------------------------------------------------------------------------------------------
export function RichApp() {
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
        <View style={{flexDirection: 'row', alignItems: 'center', gap: 16}}>
          <HeaderClock theme={theme} onPress={openSettings} />
          <CogButton onPress={openSettings} />
        </View>
      </View>

      <View style={{flex: 1, alignItems: 'center', justifyContent: 'center'}}>
        {/* The dial fills this box; the two corner steppers render after it, so they sit on top and take
            their own touches instead of starting a drag. */}
        <View style={{width: BOX_W, height: BOX_H}}>
          <ThermoDial
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
