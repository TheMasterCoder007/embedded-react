import { useState, useRef, useCallback, memo } from 'react';
import { View, Text, Pressable, Image, StyleSheet, Svg, Circle, Arc, updateVector, updateText } from 'embedded-react';
// Weather icons — the bundler's asset plugin turns each import into its baked asset name (the PNG's
// basename), so <Image source={wxSun}> resolves to the "wx_sun" buffer registered at boot. `npm run
// build` decodes the PNGs and emits them into dist/assets.generated.c (er_register_assets()).
import wxSun from './assets/wx_sun.png';
import wxCloud from './assets/wx_cloud.png';
import wxPartly from './assets/wx_partly.png';
import wxRain from './assets/wx_rain.png';

// Thermostat arc dial — a climate control built around a draggable 270° arc (a physical-thermostat
// metaphor). Dragging the handle around the arc sets the target temperature.
//

// ----------------------------------------------------------------------------------------------------
// Domain constants (from the spec's reference values)
// ----------------------------------------------------------------------------------------------------
const MIN = 45; // °F
const MAX = 90;
const STEP = 1;
const SWEEP = 270; // arc covers 270°, with a 90° gap at the bottom
const A_START = -135; // bottom-left, degrees clockwise from 12 o'clock
const DEG = SWEEP / (MAX - MIN); // degrees per °F (folds to 6) — used by the compact inline dial
const DEADBAND = 0.5; // prevents Heating/Cooling flicker when target ≈ current
const CURRENT = 68.7; // live room reading °F (static in this demo)
const ACCENT = '#f4a261'; // dial accent (amber). The compact (AOT) dial uses a STATIC arc/handle color —
//                           dynamic vector paint isn't in the compile-time subset — so it stays amber there.

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
// Responsive sizing — driven by the `screen` global (the Flow A host injects it at runtime; the Flow B
// AOT compiler seeds it from ER_AOT_SCREEN_W/H at build time). Because every term below folds to a
// constant for a given screen, the AOT picks ONE layout per board: the compact branch on a 240×320 panel
// (the only branch it compiles — the weather/drag code in the wide branch is never reached), the wide
// branch on an 800×480+ board. `screen.width` must stay foldable here (no `typeof` guard) so the compiler
// can resolve it; the Flow A host always provides `screen`.
// ----------------------------------------------------------------------------------------------------
const SW = screen.width;
const compact = SW < 400;
// Wide enough to place the thermostat + weather panel side by side (else stack them, e.g., in portrait).
const wide = !compact && SW >= 760;

// Font sizes snap to the engine's baked Inter sizes (10/12/16/20/24/32/48), so pick from that set.
const SZ = compact
  ? { R: 72, stroke: 12, handle: 10, big: 32, title: 16, sub: 12, label: 10, metric: 16, mode: 12 }
  : { R: 104, stroke: 14, handle: 12, big: 48, title: 24, sub: 16, label: 12, metric: 24, mode: 16 };

const PAD = compact ? 10 : 20;
const GAP = compact ? 8 : 16;
const BOX = 2 * (SZ.R + SZ.handle + 6); // square that holds the dial; center at (BOX/2, BOX/2)
const DIAL_C = BOX / 2;

// Node-local bbox of just the change when the value goes a -> b: the swept arc sector plus the handle,
// padded for the stroke half-width and AA. Handed to the engine, so a drag step repaints only this area
// of the dial, not the whole box.
function dirtyRectFor(a, b) {
  const a0 = angleForValue(Math.min(a, b));
  const a1 = angleForValue(Math.max(a, b));
  let minx = Infinity;
  let miny = Infinity;
  let maxx = -Infinity;
  let maxy = -Infinity;
  const steps = Math.max(1, Math.ceil((a1 - a0) / 12));
  for (let i = 0; i <= steps; i++) {
    const p = pointOnArc(a0 + ((a1 - a0) * i) / steps, DIAL_C, DIAL_C, SZ.R);
    if (p.x < minx) minx = p.x;
    if (p.y < miny) miny = p.y;
    if (p.x > maxx) maxx = p.x;
    if (p.y > maxy) maxy = p.y;
  }
  const m = SZ.stroke / 2 + SZ.handle + 4; // half stroke + handle radius + AA pad
  return [Math.floor(minx - m), Math.floor(miny - m), Math.ceil(maxx - minx + 2 * m), Math.ceil(maxy - miny + 2 * m)];
}

// Reused shape buffers for the drag hot path. Rebuilding these objects/arrays on every pointer move was
// a measurable slice of the JS dispatch cost, so they are mutated in place instead.
const _track = { arc: [DIAL_C, DIAL_C, SZ.R, A_START, -A_START], stroke: theme.track, strokeWidth: SZ.stroke, cap: 'round' };
const _prog = { arc: [DIAL_C, DIAL_C, SZ.R, A_START, 0], stroke: theme.track, strokeWidth: SZ.stroke, cap: 'round' };
const _knob = { circle: [0, 0, SZ.handle], fill: theme.card, stroke: theme.track, strokeWidth: 3 };
const _dialShapes = [];

// Build the dial's shapes for (continuous) value v — track arc, mode-colored progress arc, and the
// handle knob. Mutates the shared buffers above (no per-move allocation) and returns the shared array.
function buildDialShapes(v, color) {
  const ang = angleForValue(v);
  const h = pointOnArc(ang, DIAL_C, DIAL_C, SZ.R);
  _dialShapes.length = 0;
  _dialShapes.push(_track);
  if (v > MIN) {
    _prog.arc[4] = ang;
    _prog.stroke = color;
    _dialShapes.push(_prog);
  }
  _knob.circle[0] = h.x;
  _knob.circle[1] = h.y;
  _knob.stroke = color;
  _dialShapes.push(_knob);
  return _dialShapes;
}

// ----------------------------------------------------------------------------------------------------
// The arc dial
// ----------------------------------------------------------------------------------------------------
function Dial({ value, current, mode, color, onValue }) {
  // Dial center in absolute screen coords, captured from onLayout (a ref, so it never re-renders).
  const centerRef = useRef({ x: 0, y: 0 });
  // Handles to the dial's vector node + the big number, for imperative (no-React) updates during drag.
  const dialRef = useRef(null);
  const numRef = useRef(null);
  // The CONTINUOUS value currently on screen (a float — the handle follows the finger sub-degree for a
  // smooth drag). Synced to committed `value` every render, advanced imperatively during a drag (no
  // setState). `shownRound` tracks the displayed whole degree so the number only re-renders when it ticks.
  const shownRef = useRef(value);
  shownRef.current = value;
  const shownRoundRef = useRef(Math.round(value));
  shownRoundRef.current = Math.round(value);

  const onLayout = (e) => {
    centerRef.current = { x: e.layout.x + e.layout.width / 2, y: e.layout.y + e.layout.height / 2 };
  };

  // pointer → continuous value (spec §2): angle from the rendered center, clamp the bottom 90° gap. The
  // drag updates the dial IMPERATIVELY at the continuous value (smooth, no stepping) with a tight
  // dirtyRect, and commits to React state only on release — so a drag never pays React's reconcile cost.
  const onTouch = (e) => {
    const c = centerRef.current;
    let theta = (Math.atan2(e.x - c.x, -(e.y - c.y)) * 180) / Math.PI; // clockwise-from-top
    theta = clamp(theta, A_START, -A_START); // bottom gap snaps to -135/+135
    const v = clamp(MIN + ((theta - A_START) / SWEEP) * (MAX - MIN), MIN, MAX); // continuous, NOT rounded
    const old = shownRef.current;
    if (Math.abs(v - old) < 0.08) return; // < ~1px of handle motion: skip
    shownRef.current = v;
    updateVector(dialRef.current, buildDialShapes(v, color), dirtyRectFor(old, v));
    // The big number only changes on whole-degree ticks (and only then damages the center).
    const r = Math.round(v);
    if (r !== shownRoundRef.current) {
      shownRoundRef.current = r;
      updateText(numRef.current, r + '°');
    }
  };

  const onTouchEnd = () => {
    if (shownRef.current !== value) onValue(shownRef.current); // commit the continuous value (no snap)
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
        <Text ref={numRef} style={{ color: theme.text, fontSize: SZ.big, fontWeight: '500' }}>{Math.round(value)}°</Text>
        <Text style={{ color: theme.subtext, fontSize: SZ.sub }}>now {current}°</Text>
      </View>
    </View>
  );
}

// ----------------------------------------------------------------------------------------------------
// Small building blocks — all memoised so a value drag (which re-renders App) never reconciles them.
// Their callbacks are stabilized with useCallback in App so the memo comparison holds.
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

// 4-day outlook — each day's icon is an imported asset (resolved to its baked name by the bundler).
const FORECAST = [
  { day: 'Thu', icon: wxSun, hi: 58 },
  { day: 'Fri', icon: wxCloud, hi: 52 },
  { day: 'Sat', icon: wxRain, hi: 49 },
  { day: 'Sun', icon: wxPartly, hi: 55 },
];

// Weather panel — the demo's showcase for baked <Image> assets. Static content; memoised so the dial
// drag never reconciles it.
const WeatherPanel = memo(function WeatherPanel() {
  return (
    <View style={styles.weatherCard}>
      <Text style={styles.wxTitle}>Outside</Text>

      {/* Current conditions: a big icon next to the reading. */}
      <View style={styles.wxNow}>
        <Image source={wxPartly} resizeMode="contain" style={styles.wxNowIcon} />
        <View style={styles.wxNowText}>
          <Text style={styles.wxNowTemp}>54°</Text>
          <Text style={{ color: theme.subtext, fontSize: SZ.sub }}>Partly cloudy</Text>
          <Text style={{ color: theme.subtext, fontSize: SZ.label }}>Humidity 44%</Text>
        </View>
      </View>

      {/* 4-day forecast: a small baked icon per day. */}
      <View style={styles.wxForecast}>
        {FORECAST.map((f) => (
          <View key={f.day} style={styles.wxDay}>
            <Text style={{ color: theme.subtext, fontSize: SZ.label }}>{f.day}</Text>
            <Image source={f.icon} resizeMode="contain" style={styles.wxDayIcon} />
            <Text style={{ color: theme.text, fontSize: SZ.metric, fontWeight: '500' }}>{f.hi}°</Text>
          </View>
        ))}
      </View>
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
  const [value, setValue] = useState(70.0); // target (°F) as a FLOAT — drag glides sub-degree, UI shows Math.round
  const [mode, setMode] = useState('heat'); // default mode

  // Compact-dial drag (also valid in Flow A): capture the dial's on-screen centre via onLayout, then map a
  // touch point to a target temperature — the angle of (touch − centre) around the dial → value. Stays in
  // the Flow B (AOT) subset: value refs for the centre + a state setter, no per-instance component hooks.
  // (The wide/Flow A layout uses the richer <Dial> with imperative redraw; these refs go unused there.)
  const cx = useRef(0);
  const cy = useRef(0);
  const onDrag = useCallback((e) => {
    const ang = (Math.atan2(e.x - cx.current, cy.current - e.y) * 180) / Math.PI; // clockwise from 12 o'clock
    const clamped = ang < A_START ? A_START : ang > -A_START ? -A_START : ang;     // snap the bottom 90° gap
    const v = MIN + ((clamped - A_START) / SWEEP) * (MAX - MIN);                   // CONTINUOUS (sub-degree) target
    setValue(v < MIN ? MIN : v > MAX ? MAX : v);
  }, []);

  // ---- Compact layout (small panels, e.g., the 240×320 CYD) -------------------------------------------
  // Self-contained and within the Flow B (AOT) subset: a STATE-DRIVEN dial (arc sweep + handle follow
  // `value` via Math.sin/cos — no drag, no per-instance component hooks), integer ± steppers, and the
  // mode row as an inlined MODES.map. The value arc + handle paint recolor with `mode` (dynamic vector
  // paint — a state-driven stroke lowered to an ARGB ternary). This is the ONLY branch the AOT compiles
  // on a 240×320 board, so nothing below it (weather/drag) is reached.
  if (compact) {
    // The value arc + handle use a per-mode accent (matches the MODES colors), inlined into both strokes —
    // a local const here wouldn't be visible to the AOT's expression compiler (state/consts, not block locals).
    return (
      <View style={styles.root}>
        <Text style={styles.cTitle}>Thermostat</Text>

        {/* The dial box doubles as the drag surface: onLayout records its screen centre, and a touch
            anywhere in it sets the target to that angle (touch-down captures it, so the finger can then
            roam the whole dial). The handle/arc are inside, so grabbing them just works. */}
        <View
          style={{ width: BOX, height: BOX }}
          onLayout={(e) => {
            cx.current = e.layout.x + e.layout.width / 2;
            cy.current = e.layout.y + e.layout.height / 2;
          }}
          onTouchStart={onDrag}
          onTouchMove={onDrag}
        >
          <Svg width={BOX} height={BOX}>
            <Arc cx={DIAL_C} cy={DIAL_C} r={SZ.R} startAngle={A_START} endAngle={-A_START} stroke={theme.track} strokeWidth={SZ.stroke} strokeLinecap="round" fill="none" />
            <Arc cx={DIAL_C} cy={DIAL_C} r={SZ.R} startAngle={A_START} endAngle={A_START + (value - MIN) * DEG} stroke={mode === 'cool' ? '#4cc9f0' : mode === 'auto' ? '#2a9d8f' : mode === 'off' ? '#7d8896' : ACCENT} strokeWidth={SZ.stroke} strokeLinecap="round" fill="none" />
            <Circle
              cx={DIAL_C + SZ.R * Math.sin(((A_START + (value - MIN) * DEG) * Math.PI) / 180)}
              cy={DIAL_C - SZ.R * Math.cos(((A_START + (value - MIN) * DEG) * Math.PI) / 180)}
              r={SZ.handle}
              fill={theme.card}
              stroke={mode === 'cool' ? '#4cc9f0' : mode === 'auto' ? '#2a9d8f' : mode === 'off' ? '#7d8896' : ACCENT}
              strokeWidth={3}
            />
          </Svg>
        </View>

        <View style={styles.cReadout}>
          <Text style={styles.cStatus}>{mode === 'off' ? 'Off' : mode !== 'cool' && value > CURRENT ? 'Heating' : mode !== 'heat' && value < CURRENT ? 'Cooling' : 'Holding'}</Text>
          <Text style={styles.cBig}>{Math.round(value)}°</Text>
          <Text style={styles.cSub}>now {CURRENT}°</Text>
        </View>

        <View style={styles.stepRow}>
          <Pressable style={styles.step} onPressIn={() => setValue(Math.max(MIN, Math.round(value) - 1))}>
            <Text style={styles.stepText}>−</Text>
          </Pressable>
          <Pressable style={styles.step} onPressIn={() => setValue(Math.min(MAX, Math.round(value) + 1))}>
            <Text style={styles.stepText}>+</Text>
          </Pressable>
        </View>

        <View style={styles.modeRow}>
          {MODES.map((m) => (
            <Pressable key={m.key} style={[styles.mode, { backgroundColor: mode === m.key ? theme.modeActiveBg : theme.card }]} onPress={() => setMode(m.key)}>
              <Text style={{ color: mode === m.key ? theme.text : theme.subtext, fontSize: SZ.mode }}>{m.label}</Text>
            </Pressable>
          ))}
        </View>
      </View>
    );
  }

  // ---- Wide / stacked layouts (Flow A only: PSRAM/SDRAM boards, the simulator, a laptop) -------------
  // Below here is the rich runtime experience — the draggable Dial and the <Image>-backed weather panel.
  // The AOT never reaches it (it folds the compact branch on the small boards it targets), so it is free
  // to use the full Flow A feature set: per-instance hooks, callback props, helper calls, baked assets.
  const color = MODES.find((m) => m.key === mode).color;

  // Stable callbacks so the memoised children below don't re-render on a value drag. The stepper rounds
  // first so a press after a fractional drag lands on a whole degree.
  const selectMode = useCallback((key) => setMode(key), []);
  const dec = useCallback(() => setValue((v) => clamp(Math.round(v) - STEP, MIN, MAX)), []);
  const inc = useCallback(() => setValue((v) => clamp(Math.round(v) + STEP, MIN, MAX)), []);

  // The thermostat column (left): header + the dial card. Its old metric row now lives in the weather
  // panel on the right.
  const thermostat = (
    <View style={styles.thermo}>
      <Header />
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
    </View>
  );

  // Two wide-side layouts from the logical screen size (compact already returned above):
  //   wide   (>=760, the 800×480 landscape panel): thermostat + weather side by side.
  //   else   (e.g., 480×800 portrait via 90° rotation): the two stacked vertically.
  if (wide) {
    return (
      <View style={styles.root}>
        <View style={styles.row}>
          {thermostat}
          <WeatherPanel />
        </View>
      </View>
    );
  }
  return (
    <View style={styles.root}>
      <View style={styles.stack}>
        {thermostat}
        <WeatherPanel />
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
  // --- Compact (AOT) inline thermostat ---
  cTitle: { color: theme.text, fontSize: SZ.title, fontWeight: 'bold', marginBottom: 10 },
  // The readout overlaps the dial: pulled up over the dial center, then padded back down past it.
  cReadout: { alignItems: 'center', marginTop: -120, marginBottom: 58, gap: 1 },
  cStatus: { color: theme.subtext, fontSize: SZ.sub },
  cBig: { color: theme.text, fontSize: SZ.big, fontWeight: 'bold' },
  cSub: { color: theme.subtext, fontSize: SZ.sub },

  // Wide layout: the thermostat + weather columns sit side by side, top-aligned.
  row: { flexDirection: 'row', gap: GAP, alignItems: 'flex-start' },
  // Portrait/stacked layout: the thermostat over the weather panel, centered.
  stack: { gap: GAP, alignItems: 'center' },
  thermo: { width: compact ? '100%' : 400, gap: GAP },
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
  stepRow: { flexDirection: 'row', gap: GAP, marginBottom: GAP },
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

  // --- Weather panel (right column when wide; below the thermostat when stacked) ---
  weatherCard: {
    width: wide ? 344 : 400,
    backgroundColor: theme.card,
    borderWidth: 1,
    borderColor: theme.cardBorder,
    borderRadius: 12,
    padding: 16,
    gap: GAP,
  },
  wxTitle: { color: theme.text, fontSize: SZ.title, fontWeight: '500' },
  wxNow: { flexDirection: 'row', alignItems: 'center', gap: 12 },
  wxNowIcon: { width: 88, height: 88 },
  wxNowText: { gap: 2 },
  wxNowTemp: { color: theme.text, fontSize: SZ.big, fontWeight: '500' },
  wxForecast: { flexDirection: 'row', gap: 8 },
  wxDay: {
    flex: 1,
    alignItems: 'center',
    backgroundColor: theme.metricBg,
    borderRadius: 8,
    paddingVertical: 8,
    gap: 4,
  },
  wxDayIcon: { width: 44, height: 44 },
});
