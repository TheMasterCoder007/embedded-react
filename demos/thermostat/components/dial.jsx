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

import {useRef, useEffect, memo} from 'react';
import {
  View,
  Text,
  Pressable,
  Svg,
  Arc,
  Circle,
  Line,
  updateVector,
  updateText,
} from 'embedded-react';

// The dial — a 240° ring carrying ONE solid arc that fills from the start of the range up to the
// setpoint, with a radial handle riding its leading edge. AUTO lights only the band between its two
// setpoints and puts a handle on each end. This is the rich Flow A dial: a drag repaints the arc, the
// handle(s), and the centre number IMPERATIVELY (updateVector / updateText) and commits to React state
// only on release, so the app never reconciles mid-drag.
//
// It is self-contained — geometry, palette, and values all arrive as props — so it imports nothing from
// the app. (The compact `solo` layout in App.jsx draws its own simpler dial: the Flow B AOT compiler
// resolves identifiers only inside the single App.jsx it parses, so it cannot reach across this module.)

// ----------------------------------------------------------------------------------------------------
// Geometry
//
// The authoring space is the design's full 320×300 canvas with the dial centred at (160, 150). The design
// itself crops to viewBox "0 34 320 232", which slices the r=134 guide ring off at the top and bottom; on a
// real panel that reads as a cut-off dial, so we keep the whole canvas. Angles are ENGINE degrees — 0° at
// 12 o'clock, growing clockwise — which is what <Arc> and the imperative `arc` descriptor both take. The
// design's angles are
// SVG degrees (0° at 3 o'clock), so they are +90 here: its 150°→390° sweep becomes 240°→480°, a 240° arc
// with a 120° dead zone centred on the bottom.
// ----------------------------------------------------------------------------------------------------
export const A0 = 240;
export const SWEEP = 240;
// The lit arc is ONE continuous shape in every mode — no gaps, flat (butt) ends. AUTO's amber→cool blend
// comes from a CONIC gradient on that single stroke rather than a row of solid segments: adjacent stroked
// arcs each flatten their centreline to chords, so their outer edges bow inward and every junction shows a
// black wedge that widens toward the rim. One shape has no junctions.
//
// The engine parameterizes a conic gradient as centre + START ANGLE, with t measured clockwise from the
// top — the dial's own angle convention, so the mapping is direct.
const GRAD_CONIC = 3;

// Minimum setpoint change (°F) before a drag repaints. The sweep is 240° over 40 °F, so 0.25 °F is 1.5°
// of arc — about 4 px of handle travel at the ring radius: below the point where a repaint is visible.
const DRAG_MIN = 0.25;
// The ring and the handles are one <Svg> node each, both well inside the engine's 16-paints-per-node
// budget, so a repaint is one updateVector call per node rather than one per shape.

/** Point on a circle at `deg` engine degrees (0° = 12 o'clock, clockwise), radius `r`. */
export function polar(cx, cy, r, deg) {
  const t = (deg * Math.PI) / 180;
  return {x: cx + r * Math.sin(t), y: cy - r * Math.cos(t)};
}

// --- Colour mixing -----------------------------------------------------------------------------------
// Used for ONE thing now: tinting the centre disc towards the active mode's accent. The ring no longer
// needs interpolated solids — a stroke can carry a real gradient (see GRAD_CONIC above), which is what
// AUTO's warm→cool band uses.
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
// VectorNode — a WRITE-ONCE <Svg>. It renders its shapes a single time and is then driven entirely by
// updateVector from the Dial's effect and drag handlers.
//
// Why: reconciling the ring's <Arc> elements on every state change cost ~480 ms of the ~1250 ms
// mode-switch stall measured on the ESP32-S3 — the app's per-node reconcile + native prop push is the
// dominant cost, and node count is the lever. memo() with referentially stable props means a mode or
// setpoint change touches ZERO of these nodes in React; only the two updateVector calls run, which
// hand the engine a flat float tape and skip the reconciler completely.
//
// The `shapes` prop is the SAME array instance the imperative path mutates, so it must never be read
// during a later render — it is only used to lay the node out once.
// ----------------------------------------------------------------------------------------------------
const VectorNode = memo(function VectorNode({nodeRef, shapes, boxW, boxH}) {
  return (
    <Svg
      ref={nodeRef}
      style={{
        position: 'absolute',
        left: 0,
        top: 0,
        width: boxW,
        height: boxH,
      }}>
      {shapes.map((sh, i) =>
        sh.arc ? (
          <Arc
            key={i}
            cx={sh.arc[0]}
            cy={sh.arc[1]}
            r={sh.arc[2]}
            startAngle={sh.arc[3]}
            endAngle={sh.arc[4]}
            fill="none"
            stroke={sh.stroke}
            strokeWidth={sh.strokeWidth}
          />
        ) : (
          <Line
            key={i}
            x1={sh.line[0]}
            y1={sh.line[1]}
            x2={sh.line[2]}
            y2={sh.line[3]}
            fill="none"
            stroke={sh.stroke}
            strokeWidth={sh.strokeWidth}
          />
        ),
      )}
    </Svg>
  );
});

// ----------------------------------------------------------------------------------------------------
// Dial
//
//   geo       pixel geometry from App ({S, boxW, boxH, cx, cy, rRing, ringW, …})
//   theme     resolved palette; accent = the current mode's accent
//   mode      cool | heat | auto | off
//   value     single-setpoint °F (cool / heat / off)
//   lo, hi    the auto-mode range, °F
//   active    'lo' | 'hi' — which auto handle the steppers and a fresh drag move
//   minF/maxF the range the sweep spans
//   gap       minimum °F kept between the AUTO low and high setpoints
//   fmt       (°F) → display string, e.g. "72°" — owns the unit conversion
//   labels    { ambient } for the centre readout
//   sz        { big, dual, small } font sizes for this layout
//   onCommit  ({v, lo, hi, which}) once, on release — AUTO commits BOTH ends, since dragging one can
//             push the other along to preserve the gap
//   onPick    ('lo' | 'hi') when a drag or tap grabs an auto handle
// ----------------------------------------------------------------------------------------------------
export function Dial({
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
  const {boxW, boxH, cx, cy, rRing, ringW, rCore, rHandIn, rHandOut, S} = geo;
  const isAuto = mode === 'auto';
  // OFF shows the bare track: no lit arc and no bar. The setpoint is still held (it is what COOL comes
  // back to), it is just not drawn — nothing is being targeted while the system is off.
  const isOff = mode === 'off';
  const angOf = f => A0 + ((f - minF) / (maxF - minF)) * SWEEP;

  // Dial centre in absolute screen coords, captured from onLayout (a ref — never triggers a render).
  const centreRef = useRef({x: 0, y: 0});
  // `false` when idle; 'lo' / 'hi' / true while a finger is down. The live values live in a ref so the
  // drag path can read and advance them synchronously, without waiting on batched state.
  const dragRef = useRef(false);
  const liveRef = useRef({v: value, lo, hi});

  const ringRef = useRef(null);
  const handRef = useRef(null);
  const bigRef = useRef(null);
  const loRef = useRef(null);
  const hiRef = useRef(null);
  // Bookkeeping for what is currently ON SCREEN, so each move can skip work that would change nothing:
  // the ring is only re-uploaded when the lit span actually moves (a touch panel reports a stream of
  // samples even under a still finger), and the centre number only when its string changes.
  const litRef = useRef(null);
  const handBoxRef = useRef(null);
  const textRef = useRef('');
  const paintedRef = useRef(false); // false until the first paint of a drag, which always runs

  // Cached shape descriptors: geometry is fixed for a given size, so the arrays are built once and each
  // update mutates only the arc's span/paint or the handles' endpoints. No fresh objects per touch event —
  // allocation and GC under QuickJS in PSRAM is the real cost on a device.
  const cacheRef = useRef(null);
  // Keyed on the palette as well as the size: the track's colour is baked in at build time and is never
  // touched by paintArc (which only repaints the lit arc), so without this a theme switch left the track
  // showing the previous theme's grey.
  if (
    !cacheRef.current ||
    cacheRef.current.S !== S ||
    cacheRef.current.tick !== theme.tick
  ) {
    // Index 0 is the unlit track, drawn once across the whole sweep and never touched again. The rest are
    // the lit arc, painted over it.
    const mkArc = (a1, a2, stroke) => ({
      arc: [cx, cy, rRing, a1, a2],
      fill: 'none',
      // A SOLID hex on purpose. A wide stroke self-overlaps where the rasterizer flattens the arc, so a
      // TRANSLUCENT one blends twice at every facet and ghosts — stray shapes on a real panel.
      stroke,
      strokeWidth: ringW,
      cap: 'butt',
    });
    const ring = [mkArc(A0, A0 + SWEEP, theme.tick), mkArc(A0, A0, accent)];
    // One plain bar per handle — a solid line, no halo and no alpha. (The design blurs a translucent
    // rect under the core; that read as a smudge on a real panel.) It is radial, so a stroked Line is
    // exactly right, and far cheaper to move than a rotated rect, which would have to be re-parsed from
    // a `d` string on every move.
    const hand = [];
    for (let i = 0; i < 2; i++)
      hand.push({
        line: [0, 0, 0, 0],
        fill: 'none',
        stroke: theme.fg,
        strokeWidth: 4 * S,
        cap: 'butt',
      });
    cacheRef.current = {S, tick: theme.tick, ring, hand};
  }
  const cache = cacheRef.current;

  /** Bounding box of the ring band swept between two angles, so a move repaints only that sector. */
  const sweptBox = (a, b) => {
    const lo = a < b ? a : b;
    const hi = a < b ? b : a;
    const rIn = rRing - ringW / 2;
    const rOut = rRing + ringW / 2;
    let x0 = 1e9;
    let y0 = 1e9;
    let x1 = -1e9;
    let y1 = -1e9;
    for (let ang = lo; ; ang += 10) {
      const at = ang > hi ? hi : ang;
      for (let k = 0; k < 2; k++) {
        const p = polar(cx, cy, k === 0 ? rIn : rOut, at);
        if (p.x < x0) x0 = p.x;
        if (p.y < y0) y0 = p.y;
        if (p.x > x1) x1 = p.x;
        if (p.y > y1) y1 = p.y;
      }
      if (at >= hi) break;
    }
    const m = 2;
    return [x0 - m, y0 - m, x1 - x0 + 2 * m, y1 - y0 + 2 * m];
  };

  /**
   * Repaints the lit arc. Returns the dirty box covering only what changed since the last paint, or null
   * when the span is unchanged. ONE shape in every mode: AUTO's amber→cool blend is a conic gradient on
   * that single stroke, not a row of segments.
   */
  const paintArc = (aLo, aHi) => {
    const prev = litRef.current;
    if (prev && prev[0] === aLo && prev[1] === aHi) return null;
    const c = cache.ring[1];
    c.arc[3] = aLo;
    c.arc[4] = aHi;
    if (isAuto) {
      // Stops sit at 0 and span/360 because t runs over the FULL turn from the start angle; the engine
      // clamps to the endpoint colours past the last stop, so the rest of the circle is irrelevant.
      c.stroke = theme.accents.heat;
      c.strokeGrad = {
        type: GRAD_CONIC,
        ax: cx,
        ay: cy,
        r: (aLo * Math.PI) / 180,
        stops: [
          {color: theme.accents.heat, offset: 0},
          {color: theme.accents.cool, offset: (aHi - aLo) / 360},
        ],
      };
    } else {
      c.stroke = accent;
      c.strokeGrad = null;
    }
    // A solid arc's start never moves (it is pinned to A0), so ONLY the sector its end swept changed.
    // Do NOT union in a box for the start: that box sits down at the 7-o'clock origin, and unioning it
    // with the sector near the handle produces a rect covering nearly the whole dial — which repainted
    // the entire ring on every touch move and dropped dragging to single-digit fps.
    // AUTO is different: its conic gradient is anchored to the span, so moving either end recolours the
    // whole arc and the union of the old and new extents is genuinely needed.
    const box = !prev
      ? sweptBox(A0, A0 + SWEEP)
      : isAuto
        ? sweptBox(prev[0] < aLo ? prev[0] : aLo, prev[1] > aHi ? prev[1] : aHi)
        : sweptBox(prev[1], aHi);
    litRef.current = [aLo, aHi];
    return box;
  };

  /** Repaints the handle bars; returns the old ∪ new dirty box so only that strip is re-rasterized. */
  const paintHand = (aOne, aTwo) => {
    const set = (i, a) => {
      const p0 = polar(cx, cy, rHandIn, a);
      const p1 = polar(cx, cy, rHandOut, a);
      const s = cache.hand[i];
      s.line[0] = p0.x;
      s.line[1] = p0.y;
      s.line[2] = p1.x;
      s.line[3] = p1.y;
      s.stroke = theme.fg;
      s.strokeWidth = 4 * S;
    };
    // Collapse an unused bar to zero length rather than parking it on the live one — overlapping strokes
    // are wasted rasterizer work and would double-blend the moment anything here is not opaque. The bbox
    // loop below skips collapsed bars, so this costs nothing. OFF passes null for BOTH: the ring shows as
    // an empty track with no bar at all.
    const collapse = i => {
      const sp = cache.hand[i].line;
      sp[0] = sp[2] = cx;
      sp[1] = sp[3] = cy;
    };
    if (aOne == null) collapse(0);
    else set(0, aOne);
    if (aTwo == null) collapse(1);
    else set(1, aTwo);
    let x0 = 1e9;
    let y0 = 1e9;
    let x1 = -1e9;
    let y1 = -1e9;
    for (let i = 0; i < cache.hand.length; i++) {
      const l = cache.hand[i].line;
      // SKIP the collapsed spare. It is parked at the dial center. Including it stretched this box
      // from the center all the way out to the bar — a ~143x143 square repainted on EVERY drag move,
      // covering the core disc and the whole center readout, instead of a small strip around the bar.
      if (l[0] === l[2] && l[1] === l[3]) continue;
      x0 = Math.min(x0, l[0], l[2]);
      y0 = Math.min(y0, l[1], l[3]);
      x1 = Math.max(x1, l[0], l[2]);
      y1 = Math.max(y1, l[1], l[3]);
    }
    if (x0 > x1) return handBoxRef.current || [0, 0, 0, 0]; // nothing drawn
    const m = 5 * S; // half the 4-wide bar, plus room for AA
    const box = [x0 - m, y0 - m, x1 - x0 + 2 * m, y1 - y0 + 2 * m];
    const prev = handBoxRef.current;
    handBoxRef.current = box;
    if (!prev) return box;
    const ux = Math.min(prev[0], box[0]);
    const uy = Math.min(prev[1], box[1]);
    return [
      ux,
      uy,
      Math.max(prev[0] + prev[2], box[0] + box[2]) - ux,
      Math.max(prev[1] + prev[3], box[1] + box[3]) - uy,
    ];
  };

  // --- Touch → temperature -----------------------------------------------------------------------
  /** Engine degrees under the finger, with the bottom dead zone snapped to the nearer end of the sweep. */
  const angleAt = e => {
    const c = centreRef.current;
    let a = (Math.atan2(e.x - c.x, c.y - e.y) * 180) / Math.PI;
    if (a < 0) a += 360;
    if (a < A0) a += 360; // → [A0, A0 + 360)
    if (a > A0 + SWEEP)
      return a - (A0 + SWEEP) < A0 + 360 - a ? A0 + SWEEP : A0;
    return a;
  };

  const apply = e => {
    const a = angleAt(e);
    // Deadband: ignore a sample that has not moved the setpoint by at least DRAG_MIN. A touch panel
    // reports a steady stream even when the finger is nearly still. EVERY sample here costs a
    // vector re-upload plus a repaint of the swept sector — measured at 14-22 ms of touch-dispatch and
    // 93-110 ms of commit per frame on the ESP32-S3, i.e., 7-8 fps. Skipping the samples that would not
    // visibly move anything is most of the drag budget back.
    if (
      dragRef.current !== true &&
      dragRef.current !== 'lo' &&
      dragRef.current !== 'hi'
    )
      return;
    const raw = minF + ((a - A0) / SWEEP) * (maxF - minF);
    const live = liveRef.current;
    const cur =
      dragRef.current === 'lo'
        ? live.lo
        : dragRef.current === 'hi'
          ? live.hi
          : live.v;
    if (paintedRef.current && Math.abs(raw - cur) < DRAG_MIN) return;
    paintedRef.current = true;
    const which =
      dragRef.current === 'lo' || dragRef.current === 'hi'
        ? dragRef.current
        : null;
    // The two AUTO setpoints stay `gap` apart. Dragging one INTO the other pushes it along rather than
    // stopping dead — so the pair slides together once they meet and only halts at the range ends.
    if (which === 'lo') {
      const lo2 = raw > maxF - gap ? maxF - gap : raw < minF ? minF : raw;
      live.lo = lo2;
      if (live.hi < lo2 + gap) live.hi = lo2 + gap;
    } else if (which === 'hi') {
      const hi2 = raw < minF + gap ? minF + gap : raw > maxF ? maxF : raw;
      live.hi = hi2;
      if (live.lo > hi2 - gap) live.lo = hi2 - gap;
    } else {
      live.v = raw;
    }

    const aOne = isAuto ? angOf(live.lo) : angOf(live.v);
    const aTwo = isAuto ? angOf(live.hi) : null;
    const arcBox = paintArc(isAuto ? aOne : A0, isAuto ? aTwo : aOne);
    if (arcBox) updateVector(ringRef.current, cache.ring, arcBox);
    updateVector(handRef.current, cache.hand, paintHand(aOne, aTwo));

    // The center number, only when its rendered string actually changes. Skipping the no-op updates keeps
    // the (centred) text box OUT of the frame's damage rect — otherwise the compositor would repaint the
    // whole center-to-rim union on every move, for no visible change.
    const s = fmt(which === 'lo' ? live.lo : which === 'hi' ? live.hi : live.v);
    if (s !== textRef.current) {
      textRef.current = s;
      updateText(
        which === 'lo'
          ? loRef.current
          : which === 'hi'
            ? hiRef.current
            : bigRef.current,
        s,
      );
    }
  };

  const onStart = e => {
    if (mode === 'off') return;
    liveRef.current = {v: value, lo, hi};
    // Do NOT clear handBoxRef here. It holds where the bar currently is (the post-render effect always
    // repaints and refreshes it), and the first paint of a gesture unions with it to ERASE the old bar.
    // Clearing it meant that the first paint dirtied only the bar's NEW box: on a drag the finger starts on
    // the bar, so the two boxes overlap, and it goes unnoticed, but on a tap that jumps the bar across the
    // ring they are disjoint, and all that erased the old one was the ring-swept sector. That sector
    // is the ring band, inset by 2 px, while the bar is radial and margined by 5*S — so a sliver of the
    // old bar fell outside it and stayed on screen.
    paintedRef.current = false;
    if (isAuto) {
      const a = angleAt(e);
      const raw = minF + ((a - A0) / SWEEP) * (maxF - minF);
      const which = Math.abs(raw - lo) <= Math.abs(raw - hi) ? 'lo' : 'hi';
      dragRef.current = which;
      textRef.current = fmt(which === 'lo' ? lo : hi);
      if (which !== active) onPick(which);
    } else {
      dragRef.current = true;
      textRef.current = fmt(value);
    }
    apply(e);
  };
  const onMove = e => {
    if (dragRef.current) apply(e);
  };
  const onEnd = () => {
    const which =
      dragRef.current === 'lo' || dragRef.current === 'hi'
        ? dragRef.current
        : null;
    if (!dragRef.current) return;
    dragRef.current = false;
    const live = liveRef.current;
    // Hand the final value to React. The re-render that follows repaints the same pixels the imperative
    // path already put on screen, so nothing moves — it just puts the declarative tree back in sync.
    onCommit({v: live.v, lo: live.lo, hi: live.hi, which});
  };

  // --- Render (mount, steppers, mode / theme change, post-drag re-sync) ---------------------------
  if (!dragRef.current) {
    // The effect below owns the pixels from here; drop the arc's span so it repaints unconditionally.
    // Only litRef: paintArc early-outs on an unchanged span, so it needs clearing, but paintHand
    // always repaints and handBoxRef is purely the record of where the bar is TO ERASE IT. Clearing it
    // here left the bar's old pixels behind every time a render moved it without a drag — the steppers,
    // which is where this last showed up. Same root cause as the tap-to-jump artifact, another path.
    litRef.current = null;
  }
  // Every state change repaints the ring and the handle through updateVector rather than through React.
  // No dep array on purpose: any render should re-sync the pixels, and the two calls are inexpensive (a flat
  // float tape, no reconciliation). A drag renders nothing, so this never runs mid-drag — the touch
  // handlers already drive the same two nodes directly.
  useEffect(() => {
    const aOne = isOff ? null : isAuto ? angOf(lo) : angOf(value);
    const aTwo = isAuto ? angOf(hi) : null;
    // OFF collapses the lit arc onto the track's start, so only the empty track is left showing.
    paintArc(isAuto ? aOne : A0, isOff ? A0 : isAuto ? aTwo : aOne);
    updateVector(ringRef.current, cache.ring);
    updateVector(handRef.current, cache.hand, paintHand(aOne, aTwo));
  });

  // Both ends are WHITE, matching the single-setpoint readout and the solo branch. The accents already
  // carry the mode on the ring and the mode row; coloring the numbers as well-made the pair read as two
  // unrelated values rather than one range. Tapping either still selects it for the steppers.
  const dualNum = (which, ref, v) => (
    <Pressable onPress={() => onPick(which)}>
      <Text
        ref={ref}
        style={{fontSize: sz.dual, fontWeight: '500', color: theme.fg}}>
        {fmt(v)}
      </Text>
    </Pressable>
  );

  return (
    <View
      style={{position: 'absolute', left: 0, top: 0, width: boxW, height: boxH}}
      onLayout={e => {
        centreRef.current = {x: e.layout.x + cx, y: e.layout.y + cy};
      }}
      onTouchStart={onStart}
      onTouchMove={onMove}
      onTouchEnd={onEnd}>
      {/* Face: a single solid accent-tinted disc. Re-renders only on a mode / theme change, never
          during a drag.
          This started as three stacked translucent discs faking the design's radial gradient, over guide
          rings at r=134, r=130 and r=84. Every one of those edges reads as its own hard circle on a real
          panel — a pile of stray shapes around the segments — so they are all gone now. The disc sits at
          r=86, leaving 20 units of clear background before the block band's 106 inner edges. */}
      <Svg
        style={{
          position: 'absolute',
          left: 0,
          top: 0,
          width: boxW,
          height: boxH,
        }}>
        {/* Pre-mixed to a SOLID hex: a View-style alpha fill would be ignored, and stacking alpha here is
            exactly what produced the banding. */}
        <Circle
          cx={cx}
          cy={cy}
          r={rCore}
          fill={mix(theme.surface, accent, 0.14)}
        />
      </Svg>

      <VectorNode
        nodeRef={ringRef}
        shapes={cache.ring}
        boxW={boxW}
        boxH={boxH}
      />
      {/* Handle(s) — their own vector node, so a drag moves them without re-uploading the ring. */}
      <VectorNode
        nodeRef={handRef}
        shapes={cache.hand}
        boxW={boxW}
        boxH={boxH}
      />

      {/* Center readout. Deliberately BOUNDED on both axes: this overlay sits above the ring in paint and
          hit-test order, so anywhere it covers is a place the drag cannot be started. At ±83 × ±60
          authoring units, its farthest corner is ~102 units from the center — just inside the ring band's
          106 inner edges — so the whole ring stays grabbable. Making it full-height (the obvious thing)
          silently kills the drag across the top and bottom of the dial. */}
      <View
        style={{
          position: 'absolute',
          left: cx - 83 * S,
          top: cy - 60 * S,
          width: 166 * S,
          height: 120 * S,
          alignItems: 'center',
          justifyContent: 'center',
          gap: 6,
        }}>
        <Text style={{fontSize: sz.small, letterSpacing: 2, color: theme.dim}}>
          {labels.ambient}
        </Text>
        {isAuto ? (
          <View style={{flexDirection: 'row', alignItems: 'flex-end', gap: 0}}>
            {dualNum('lo', loRef, lo)}
            {/* Sized with the numbers, not with the captions: as a caption-sized glyph between two
                readouts three times its height, it read as a stray mark rather than a separator. Set
                tight against both numbers (the row's gap is 0) so the pair reads as one range. */}
            <Text
              style={{fontSize: sz.dual, color: theme.dim, marginBottom: 1}}>
              -
            </Text>
            {dualNum('hi', hiRef, hi)}
          </View>
        ) : (
          <Text
            ref={bigRef}
            style={{fontSize: sz.big, fontWeight: '500', color: theme.fg}}>
            {mode === 'off' ? '––' : fmt(value)}
          </Text>
        )}
      </View>
    </View>
  );
}
