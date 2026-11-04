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
  BOX_HEIGHT,
  BOX_WIDTH,
  ROW_GAP,
  CARD_PADDING,
  CARD_GAP,
  DIAL_GEOMETRY,
  HEADER_HEIGHT,
  INSIDE_FAHRENHEIT,
  LAYOUT,
  MAX_FAHRENHEIT,
  MIN_FAHRENHEIT,
  MODES,
  MODE_ROW_HEIGHT,
  OVERLAY,
  ROOT_PADDING,
  RANGE_GAP,
  SCREEN_HEIGHT,
  SCREEN_WIDTH,
  FONT_SIZES,
  THERMOSTAT_OUTER_HEIGHT,
  THERMOSTAT_OUTER_WIDTH,
  WEATHER_OUTER_HEIGHT,
  WEATHER_OUTER_WIDTH,
  WEATHER_WIDTH,
} from './App.jsx';

// The thermostat's split and stack layouts in Flow A, on the real JS engine: the imperative drag dial, the
// <Image>-backed weather panel, and the theme + units sheet. Flow B doesn't compile this file: App.jsx is its
// whole app. index.jsx picks between the two.

// Palettes. Values the design writes as rgba() are pre-mixed to SOLID hex wherever they land on a View
// background or Text color — the engine ignores alpha on a View fill. Alpha survives on borders and on
// vector paints, so `line` / `tick` (both strokes or borders) keep theirs.
const THEMES = {
  dark: {
    background: '#0a0b0c',
    surface: '#131517',
    foreground: '#f2f3f4',
    dim: '#7d7e7f',
    secondary: '#afafb0',
    line: '#ffffff1a',
    tick: '#26282a', // SOLID: see the stroke note in components/dial.jsx
    trackBackground: '#26282a',
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
    background: '#eeeeec',
    surface: '#ffffff',
    foreground: '#101112',
    dim: '#8a8a8a',
    secondary: '#525252',
    line: '#0000001c',
    tick: '#ededed', // SOLID: see the stroke note in components/dial.jsx
    trackBackground: '#ededed',
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
const CARD_STYLE = {};
const WEATHER_STYLE = {};
for (const themeKey of ['dark', 'light']) {
  CARD_STYLE[themeKey] = {
    backgroundColor: THEMES[themeKey].surface,
    borderWidth: 1,
    borderColor: THEMES[themeKey].line,
    borderRadius: 18,
    padding: CARD_PADDING,
  };
  WEATHER_STYLE[themeKey] = {
    ...CARD_STYLE[themeKey],
    width: WEATHER_OUTER_WIDTH,
    height: WEATHER_OUTER_HEIGHT,
    gap: 14,
  };
}
// The forecast row's flexible column: the card's inner width less the fixed columns and the five 12 px gaps.
const BAR_WIDTH = Math.max(
  24,
  WEATHER_WIDTH - (46 + 24 + 36 + 30 + 30) - 12 * 5,
);

// A stepper press in °C moves half a degree.
const HALF_CELSIUS_IN_FAHRENHEIT = 0.9;

const clampF = fahrenheit =>
  fahrenheit < MIN_FAHRENHEIT
    ? MIN_FAHRENHEIT
    : fahrenheit > MAX_FAHRENHEIT
      ? MAX_FAHRENHEIT
      : fahrenheit;

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
const SEGMENT = {};
const SEGMENT_TEXT = {};
const SHEET_STYLE = {};
const SHEET_WIDE_STYLE = {}; // with the network column beside the rest
for (const themeKey of ['dark', 'light']) {
  const theme = THEMES[themeKey];
  STEP_LEFT[themeKey] = {
    position: 'absolute',
    left: 0,
    bottom: 0,
    width: 52,
    height: 52,
    borderRadius: 26,
    borderWidth: 1,
    borderColor: theme.line,
    backgroundColor: theme.surface,
    alignItems: 'center',
    justifyContent: 'center',
  };
  STEP_RIGHT[themeKey] = {...STEP_LEFT[themeKey], left: undefined, right: 0};
  const seg = on => ({
    flex: 1,
    height: 44,
    borderRadius: 9,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: on ? theme.foreground : theme.trackBackground,
  });
  SEGMENT[themeKey] = {on: seg(true), off: seg(false)};
  SEGMENT_TEXT[themeKey] = {
    on: {fontSize: 12, letterSpacing: 1, color: theme.background},
    off: {fontSize: 12, letterSpacing: 1, color: theme.dim},
  };
  SHEET_STYLE[themeKey] = {
    width: 340,
    backgroundColor: theme.surface,
    borderWidth: 1,
    borderColor: theme.line,
    borderRadius: 16,
    padding: 22,
    gap: 20,
  };
  SHEET_WIDE_STYLE[themeKey] = {...SHEET_STYLE[themeKey], width: 664};
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

const Stepper = memo(function Stepper({onPress, style, minus, color}) {
  return (
    <Pressable onPress={onPress} style={style}>
      {minus ? (
        <View style={{width: 16, height: 2, backgroundColor: color}} />
      ) : (
        <Text style={{fontSize: 20, color: color}}>+</Text>
      )}
    </Pressable>
  );
});

// Only the two buttons whose `on` actually flips re-render on a mode change.
const ModeButton = memo(function ModeButton({item, on, themeName, onSelect}) {
  const theme = THEMES[themeName];
  return (
    <Pressable
      onPress={() => onSelect(item.key)}
      style={{
        flex: 1,
        height: MODE_ROW_HEIGHT,
        borderRadius: 10,
        borderWidth: 1,
        borderColor: on ? theme.accents[item.key] : theme.line,
        backgroundColor: on ? theme.tints[item.key] : theme.surface,
        alignItems: 'center',
        justifyContent: 'center',
      }}>
      <Text
        style={{
          fontSize: FONT_SIZES.mode,
          letterSpacing: 1,
          color: on ? theme.accents[item.key] : theme.dim,
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
  const theme = THEMES[themeName];
  const segment = SEGMENT[themeName];
  const segmentText = SEGMENT_TEXT[themeName];
  // The Wi-Fi and time-zone pages take the sheet's place while one is open.
  const [page, setPage] = useState(null);
  const back = useCallback(() => setPage(null), []);
  if (page) {
    return (
      <Modal visible={visible} backdropColor={theme.scrim} style={OVERLAY}>
        {page === 'wifi' ? (
          <WifiPage theme={theme} onDone={back} />
        ) : (
          <ZonePage theme={theme} onDone={back} />
        )}
      </Modal>
    );
  }
  return (
    <Modal visible={visible} backdropColor={theme.scrim} style={OVERLAY}>
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
              color: theme.foreground,
            }}>
            SETTINGS
          </Text>
          <Pressable onPress={onClose} style={CLOSE_HIT}>
            <Text style={{fontSize: 24, color: theme.dim}}>×</Text>
          </Pressable>
        </View>

        <Columns
          wide={HAS_NETWORK}
          side={<NetworkRows theme={theme} onOpen={setPage} />}>
          <View style={{gap: 9}}>
            <Text style={{fontSize: 10, letterSpacing: 2, color: theme.dim}}>
              THEME
            </Text>
            <View style={{flexDirection: 'row', gap: 4}}>
              <Pressable
                onPress={() => onTheme('dark')}
                style={themeName === 'dark' ? segment.on : segment.off}>
                <Text
                  style={
                    themeName === 'dark' ? segmentText.on : segmentText.off
                  }>
                  DARK
                </Text>
              </Pressable>
              <Pressable
                onPress={() => onTheme('light')}
                style={themeName === 'light' ? segment.on : segment.off}>
                <Text
                  style={
                    themeName === 'light' ? segmentText.on : segmentText.off
                  }>
                  LIGHT
                </Text>
              </Pressable>
            </View>
          </View>

          <View style={{gap: 9}}>
            <Text style={{fontSize: 10, letterSpacing: 2, color: theme.dim}}>
              UNITS
            </Text>
            <View style={{flexDirection: 'row', gap: 4}}>
              <Pressable
                onPress={() => onUnit('F')}
                style={unit === 'F' ? segment.on : segment.off}>
                <Text style={unit === 'F' ? segmentText.on : segmentText.off}>
                  °F
                </Text>
              </Pressable>
              <Pressable
                onPress={() => onUnit('C')}
                style={unit === 'C' ? segment.on : segment.off}>
                <Text style={unit === 'C' ? segmentText.on : segmentText.off}>
                  °C
                </Text>
              </Pressable>
            </View>
          </View>

          <ClockSetter theme={theme} />
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

  const theme = THEMES[themeName];
  const accent = theme.accents[mode];

  // °F → display string. °C is quantized to half a degree, matching the design.
  const fmt = fahrenheit => {
    if (unit === 'C') {
      const celsius = Math.round(((fahrenheit - 32) * 5) / 9 / 0.5) * 0.5;
      return (
        (Number.isInteger(celsius) ? celsius.toFixed(0) : celsius.toFixed(1)) +
        '°'
      );
    }
    return Math.round(fahrenheit) + '°';
  };
  const fmtBare = fahrenheit =>
    unit === 'C'
      ? String(Math.round(((fahrenheit - 32) * 5) / 9))
      : String(Math.round(fahrenheit));

  // A stepper press moves whichever setpoint is live by 1 °F (or half a °C). In AUTO the pair stays
  // RANGE_GAP apart by PUSHING the far handle along, matching the drag behavior.
  const step = direction => {
    const increment = unit === 'C' ? HALF_CELSIUS_IN_FAHRENHEIT : 1;
    if (mode === 'off') return;
    if (mode === 'auto') {
      if (active === 'lo') {
        const nextLow = clampF(
          Math.min(low + direction * increment, MAX_FAHRENHEIT - RANGE_GAP),
        );
        setLow(nextLow);
        if (high < nextLow + RANGE_GAP) setHigh(nextLow + RANGE_GAP);
      } else {
        const nextHigh = clampF(
          Math.max(high + direction * increment, MIN_FAHRENHEIT + RANGE_GAP),
        );
        setHigh(nextHigh);
        if (low > nextHigh - RANGE_GAP) setLow(nextHigh - RANGE_GAP);
      }
      return;
    }
    const nextSetpoint = clampF(Math.round(value) + direction * increment);
    if (mode === 'heat') setHeatSetpoint(nextSetpoint);
    else setCoolSetpoint(nextSetpoint);
  };
  // Ref-backed so these keep the same identity across renders — which is what lets the memoised
  // steppers, cog and mode buttons skip re-rendering.
  const stepRef = useRef(null);
  stepRef.current = step;
  const stepDown = useCallback(() => stepRef.current(-1), []);
  const stepUp = useCallback(() => stepRef.current(1), []);
  const openSettings = useCallback(() => setSettings(true), []);
  const closeSettings = useCallback(() => setSettings(false), []);

  // AUTO commits BOTH ends: dragging one can have pushed the other to preserve the gap, and the dial
  // already enforced it live, so these arrive valid.
  const commit = committed => {
    if (mode === 'auto') {
      setLow(clampF(committed.low));
      setHigh(clampF(committed.high));
      return;
    }
    const setpoint = clampF(committed.value);
    if (mode === 'heat') setHeatSetpoint(setpoint);
    else setCoolSetpoint(setpoint);
  };

  // What the equipment is actually doing, from the room reading against the target. It only RUNS while
  // it still has somewhere to go: COOL while the room is above its setpoint, HEAT while it is below,
  // AUTO outside the band either way. Once the target is met nothing is running, so it reads IDLE — the
  // fan stays on AUTO throughout, which is what a real stat shows when it is satisfied.
  const running =
    mode === 'cool'
      ? INSIDE_FAHRENHEIT > coolSetpoint
      : mode === 'heat'
        ? INSIDE_FAHRENHEIT < heatSetpoint
        : mode === 'auto'
          ? INSIDE_FAHRENHEIT > high || INSIDE_FAHRENHEIT < low
          : false;
  const statusLine = running ? 'RUNNING • FAN AUTO' : 'IDLE • FAN AUTO';

  const card = CARD_STYLE[themeName];

  const thermostat = (
    <View
      style={{
        ...card,
        width: THERMOSTAT_OUTER_WIDTH,
        height: THERMOSTAT_OUTER_HEIGHT,
        gap: ROW_GAP,
      }}>
      <View
        style={{
          height: HEADER_HEIGHT,
          flexDirection: 'row',
          alignItems: 'center',
          justifyContent: 'space-between',
        }}>
        <View style={{gap: 3}}>
          <Text
            style={{
              fontSize: FONT_SIZES.room,
              fontWeight: '600',
              letterSpacing: 1,
              color: theme.foreground,
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
        <View style={{width: BOX_WIDTH, height: BOX_HEIGHT}}>
          <ThermoDial
            geometry={DIAL_GEOMETRY}
            theme={theme}
            accent={accent}
            mode={mode}
            value={value}
            low={low}
            high={high}
            active={active}
            minimum={MIN_FAHRENHEIT}
            maximum={MAX_FAHRENHEIT}
            gap={RANGE_GAP}
            fmt={fmt}
            labels={{ambient: 'INSIDE ' + fmt(INSIDE_FAHRENHEIT)}}
            fontSizes={FONT_SIZES}
            onCommit={commit}
            onPick={setActive}
          />
          <Stepper
            onPress={stepDown}
            style={STEP_LEFT[themeName]}
            minus
            color={theme.foreground}
          />
          <Stepper
            onPress={stepUp}
            style={STEP_RIGHT[themeName]}
            color={theme.foreground}
          />
        </View>
      </View>

      <View style={{height: MODE_ROW_HEIGHT, flexDirection: 'row', gap: 6}}>
        {MODES.map(item => (
          <ModeButton
            key={item.key}
            item={item}
            on={mode === item.key}
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
      barWidth={BAR_WIDTH}
      style={WEATHER_STYLE[themeName]}
    />
  );

  return (
    <View
      style={{
        width: SCREEN_WIDTH,
        height: SCREEN_HEIGHT,
        backgroundColor: theme.background,
        padding: ROOT_PADDING,
        flexDirection: LAYOUT === 'split' ? 'row' : 'column',
        gap: CARD_GAP,
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
