import { useState, useRef, useMemo, useCallback, memo } from 'react';
import { View, Text, Pressable, StyleSheet, Svg, Circle, Arc, updateVector, updateText } from 'embedded-react';

// Thermostat arc dial — a climate control built around a draggable 270° arc (a physical-thermostat
// metaphor). Dragging the handle around the arc sets the target temperature.
//

// ----------------------------------------------------------------------------------------------------
// Domain constants (from the spec's reference values)
// ----------------------------------------------------------------------------------------------------
const MIN = 16; // °C
const MAX = 28;
const STEP = 1;
const SWEEP = 270; // arc covers 270°, with a 90° gap at the bottom
const A_START = -135; // bottom-left, degrees clockwise from 12 o'clock
const DEADBAND = 0.3; // prevents Heating/Cooling flicker when target ≈ current
const CURRENT = 20.4; // live room reading (static in this demo)

const MODES = [
  { key: 'heat', label: 'Heat', color: '#f4a261' }, // warning / amber
  { key: 'cool', label: 'Cool', color: '#4cc9f0' }, // info / blue
  { key: 'auto', label: 'Auto', color: '#2a9d8f' }, // success / green
  { key: 'off', label: 'Off', color: '#7d8896' }, // muted / gray
];

// Color tokens — centralized so the whole UI is themeable (spec §6: drive colors from tokens, never
// hardcode per-screen hex). This is the dark theme the device ships with.
const theme = {
  bg: '#0e1521',
  card: '#16202f',
  cardBorder: '#26344a',
  metricBg: '#1b2738',
  text: '#e7edf5',
  subtext: '#9aa5b1',
  track: '#2c3a4f', // inactive arc dots
  modeActiveBg: '#26344a',
  modeBorder: '#2c3a4f',
};

// ----------------------------------------------------------------------------------------------------
// Geometry helpers (spec §2)
// ----------------------------------------------------------------------------------------------------
function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

// value → angle (degrees, clockwise from top). MIN → -135°, MAX → +135°.
function angleForValue(v) {
  return A_START + ((v - MIN) / (MAX - MIN)) * SWEEP;
}

// A point on the arc. The minus on cos puts 0° at the top and grows y downward (SVG convention).
function pointOnArc(angleDeg, cx, cy, r) {
  const t = (angleDeg * Math.PI) / 180;
  return { x: cx + r * Math.sin(t), y: cy - r * Math.cos(t) };
}

// Center status word from target vs. current (spec §3). The deadband stops flicker near equality.
function statusFor(mode, value, current) {
  if (mode === 'off') return 'Off';
  if (mode !== 'cool' && value > current + DEADBAND) return 'Heating';
  if (mode !== 'heat' && value < current - DEADBAND) return 'Cooling';
  return 'Holding';
}

// ----------------------------------------------------------------------------------------------------
// Responsive sizing — read once from the host-injected `screen` global.
// ----------------------------------------------------------------------------------------------------
const SW = (typeof screen !== 'undefined' && screen.width) || 800;
const compact = SW < 400;

// Font sizes snap to the engine's baked Inter sizes (10/12/16/20/24/32/48), so pick from that set.
const SZ = compact
  ? { R: 52, stroke: 8, handle: 9, big: 32, title: 16, sub: 12, label: 10, metric: 16, mode: 12 }
  : { R: 104, stroke: 14, handle: 12, big: 48, title: 24, sub: 16, label: 12, metric: 24, mode: 16 };

const PAD = compact ? 10 : 20;
const GAP = compact ? 8 : 16;
const BOX = 2 * (SZ.R + SZ.handle + 4); // square that holds the dial; center at (BOX/2, BOX/2)
const DIAL_C = BOX / 2;

// ----------------------------------------------------------------------------------------------------
// The arc dial
// ----------------------------------------------------------------------------------------------------
function Dial({ value, current, mode, color, onValue }) {
  // Dial center in absolute screen coords, captured from onLayout (a ref, so it never re-renders).
  const centerRef = useRef({ x: 0, y: 0 });
  // Handles to the dial's vector node + the big number, for imperative (no-React) updates during drag.
  const dialRef = useRef(null);
  const numRef = useRef(null);
  // The value currently on screen. Synced to the committed `value` every render, and advanced
  // imperatively during a drag (without setState) so dragging never runs React's reconcile.
  const shownRef = useRef(value);
  shownRef.current = value;

  const onLayout = (e) => {
    centerRef.current = { x: e.layout.x + e.layout.width / 2, y: e.layout.y + e.layout.height / 2 };
  };

  // Build the dial's shapes for value v and push them straight to the nodes — no React, no d-string
  // parsing (the <Arc> primitive maps to a native arc op). Cheap enough to call on every pointer move.
  const drawDial = (v) => {
    const h = pointOnArc(angleForValue(v), DIAL_C, DIAL_C, SZ.R);
    const shapes = [
      { arc: [DIAL_C, DIAL_C, SZ.R, A_START, -A_START], stroke: theme.track, strokeWidth: SZ.stroke, cap: 'round' },
    ];
    if (v > MIN) {
      shapes.push({
        arc: [DIAL_C, DIAL_C, SZ.R, A_START, angleForValue(v)],
        stroke: color,
        strokeWidth: SZ.stroke,
        cap: 'round',
      });
    }
    shapes.push({ circle: [h.x, h.y, SZ.handle], fill: theme.card, stroke: color, strokeWidth: 3 });
    updateVector(dialRef.current, shapes);
    updateText(numRef.current, v + '°');
  };

  // pointer → value (spec §2): angle from the rendered center, clamp the bottom 90° gap to the nearest
  // end, snap to step. The drag updates the dial IMPERATIVELY (drawDial) and only commits to React state
  // on release (onTouchEnd) — so a continuous drag never pays React's reconcile cost in PSRAM-QuickJS.
  const onTouch = (e) => {
    const c = centerRef.current;
    let theta = (Math.atan2(e.x - c.x, -(e.y - c.y)) * 180) / Math.PI; // clockwise-from-top
    theta = clamp(theta, A_START, -A_START); // bottom gap snaps to -135/+135
    const v = clamp(Math.round(MIN + ((theta - A_START) / SWEEP) * (MAX - MIN)), MIN, MAX);
    if (v === shownRef.current) return; // same step — nothing to redraw
    shownRef.current = v;
    drawDial(v);
  };

  const onTouchEnd = () => {
    if (shownRef.current !== value) onValue(shownRef.current); // commit the drag result to React
  };

  const handle = pointOnArc(angleForValue(value), DIAL_C, DIAL_C, SZ.R);

  return (
    <View
      onLayout={onLayout}
      onTouchStart={onTouch}
      onTouchMove={onTouch}
      onTouchEnd={onTouchEnd}
      style={{ width: BOX, height: BOX }}
    >
      {/* The dial is ONE vector node: a muted track arc, the mode-colored progress arc, and the handle
          knob. At REST React renders it declaratively; during a DRAG we update it imperatively through
          dialRef (drawDial), bypassing React's reconcile — the expensive part on a PSRAM device. */}
      <Svg ref={dialRef} style={{ position: 'absolute', left: 0, top: 0, width: BOX, height: BOX }}>
        <Arc cx={DIAL_C} cy={DIAL_C} r={SZ.R} startAngle={A_START} endAngle={-A_START} stroke={theme.track} strokeWidth={SZ.stroke} strokeLinecap="round" fill="none" />
        {value > MIN && (
          <Arc cx={DIAL_C} cy={DIAL_C} r={SZ.R} startAngle={A_START} endAngle={angleForValue(value)} stroke={color} strokeWidth={SZ.stroke} strokeLinecap="round" fill="none" />
        )}
        {/* Handle: a knob in the card color with a 3px stroke in the mode color (spec §5). */}
        <Circle cx={handle.x} cy={handle.y} r={SZ.handle} fill={theme.card} stroke={color} strokeWidth={3} />
      </Svg>

      {/* Center readout, absolutely centered over the dial. The big number has a ref so the drag can
          update it imperatively too (updateText keeps its style). */}
      <View
        style={{
          position: 'absolute',
          left: 0,
          top: 0,
          width: BOX,
          height: BOX,
          alignItems: 'center',
          justifyContent: 'center',
        }}
      >
        <Text style={{ color: theme.subtext, fontSize: SZ.sub }}>{statusFor(mode, value, current)}</Text>
        <Text ref={numRef} style={{ color: theme.text, fontSize: SZ.big, fontWeight: '500' }}>{value}°</Text>
        <Text style={{ color: theme.subtext, fontSize: SZ.sub }}>now {current}°</Text>
      </View>
    </View>
  );
}

// ----------------------------------------------------------------------------------------------------
// Small building blocks — all memoised so a value drag (which re-renders App) never reconciles them.
// Their callbacks are stabilised with useCallback in App so the memo comparison holds.
// ----------------------------------------------------------------------------------------------------
const StepButton = memo(function StepButton({ label, onPress }) {
  return (
    <Pressable onPress={onPress} style={styles.step}>
      <Text style={styles.stepText}>{label}</Text>
    </Pressable>
  );
});

const ModeButton = memo(function ModeButton({ item, active, onSelect }) {
  // Active = filled secondary bg + stronger text; inactive = outline. Color lives in the arc, NOT the
  // active button (spec §5). Only the two buttons whose `active` flips re-render on a mode switch.
  return (
    <Pressable onPress={() => onSelect(item.key)} style={[styles.mode, active ? styles.modeActive : styles.modeIdle]}>
      <Text style={{ color: active ? theme.text : theme.subtext, fontSize: SZ.mode }}>{item.label}</Text>
    </Pressable>
  );
});

const Metric = memo(function Metric({ label, value }) {
  return (
    <View style={styles.metric}>
      <Text style={{ color: theme.subtext, fontSize: SZ.label }}>{label}</Text>
      <Text style={{ color: theme.text, fontSize: SZ.metric, fontWeight: '500' }}>{value}</Text>
    </View>
  );
});

// Header never changes after mount — memoise it so it doesn't reconcile on every value change.
const Header = memo(function Header() {
  return (
    <View style={styles.header}>
      <View>
        <Text style={{ color: theme.text, fontSize: SZ.title, fontWeight: '500' }}>Thermostat</Text>
        <Text style={{ color: theme.subtext, fontSize: SZ.sub }}>Living room</Text>
      </View>
      <View style={styles.pill}>
        <Text style={{ color: theme.subtext, fontSize: SZ.label }}>{compact ? 'Eco on' : 'Eco schedule on'}</Text>
      </View>
    </View>
  );
});

// ----------------------------------------------------------------------------------------------------
// App
// ----------------------------------------------------------------------------------------------------
export function App() {
  const [value, setValue] = useState(21); // default target (spec §8)
  const [mode, setMode] = useState('heat'); // default mode

  const color = useMemo(() => MODES.find((m) => m.key === mode).color, [mode]);

  // Stable callbacks so the memoised children below don't re-render on a value drag.
  const selectMode = useCallback((key) => setMode(key), []);
  const dec = useCallback(() => setValue((v) => clamp(v - STEP, MIN, MAX)), []);
  const inc = useCallback(() => setValue((v) => clamp(v + STEP, MIN, MAX)), []);

  return (
    <View style={styles.root}>
      <View style={styles.content}>
        <Header />

        {/* Dial card */}
        <View style={styles.card}>
          <Dial value={value} current={CURRENT} mode={mode} color={color} onValue={setValue} />

          {/* Stepper row */}
          <View style={styles.stepRow}>
            <StepButton label="−" onPress={dec} />
            <StepButton label="+" onPress={inc} />
          </View>

          {/* Mode selector */}
          <View style={styles.modeRow}>
            {MODES.map((m) => (
              <ModeButton key={m.key} item={m} active={mode === m.key} onSelect={selectMode} />
            ))}
          </View>
        </View>

        {/* Metric row */}
        <View style={styles.metricRow}>
          <Metric label="Humidity" value="44%" />
          <Metric label="Outdoor" value="12°" />
          <Metric label="Next change" value="18° at 11pm" />
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: theme.bg,
    paddingHorizontal: PAD,
    paddingVertical: PAD,
    alignItems: 'center',
    justifyContent: compact ? 'flex-start' : 'center',
  },
  // Cap the column width on the wide screen so it reads as a tidy panel rather than stretching.
  content: { width: compact ? '100%' : 440, gap: GAP },
  header: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  pill: {
    backgroundColor: theme.metricBg,
    borderRadius: 999,
    paddingHorizontal: 10,
    paddingVertical: 4,
  },
  card: {
    backgroundColor: theme.card,
    borderWidth: 1,
    borderColor: theme.cardBorder,
    borderRadius: 12,
    padding: compact ? 8 : 16,
    gap: GAP,
    alignItems: 'center',
  },
  stepRow: { flexDirection: 'row', gap: GAP },
  step: {
    width: compact ? 56 : 72,
    height: compact ? 32 : 40,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: theme.modeBorder,
    alignItems: 'center',
    justifyContent: 'center',
  },
  stepText: { color: theme.text, fontSize: compact ? 20 : 24 },
  modeRow: { flexDirection: 'row', gap: compact ? 6 : 8, alignSelf: 'stretch' },
  mode: {
    flex: 1,
    height: compact ? 30 : 40,
    borderRadius: 8,
    alignItems: 'center',
    justifyContent: 'center',
  },
  modeIdle: { borderWidth: 1, borderColor: theme.modeBorder },
  modeActive: { backgroundColor: theme.modeActiveBg },
  metricRow: { flexDirection: 'row', gap: compact ? 6 : 10 },
  metric: {
    flex: 1,
    backgroundColor: theme.metricBg,
    borderRadius: 8,
    padding: compact ? 8 : 12,
    gap: 2,
  },
});
