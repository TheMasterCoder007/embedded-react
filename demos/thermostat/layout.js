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

// The thermostat's layout and model: which of the three layouts the panel gets, the sizes worked out from it, and
// the values both apps share. App.jsx and RichApp.jsx import them from here.

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
export const SOLO = LAYOUT === 'solo';

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
export const LANDSCAPE = SOLO && SCREEN_WIDTH > SCREEN_HEIGHT;
export const MODE_COLUMN_WIDTH = 56; // landscape: the mode column's width
export const ARC_ROW_HEIGHT = LANDSCAPE
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
export const SCALE = BOX_WIDTH / AUTHORING_WIDTH; // authoring unit → screen px

export const DIAL_CENTER_X = 160 * SCALE;
export const DIAL_CENTER_Y = 150 * SCALE;
const DIAL_RING_RADIUS = 116 * SCALE;
export const DIAL_RING_WIDTH = 20 * SCALE;
const DIAL_HANDLE_INNER_RADIUS = 97 * SCALE;
const DIAL_HANDLE_OUTER_RADIUS = 135 * SCALE;
export const DIAL_CORE_RADIUS = 86 * SCALE; // the solid center disc — 20 units of clearance to the blocks' 106 inner edge
export const DIAL_SIZE = 2 * (116 + 10) * SCALE;

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
export const SWEEP_ANGLE = 240;
// <Dial> measures its own angles clockwise from 3 o'clock, so the design's START_ANGLE shifts by a quarter turn.
export const DIAL_START_ANGLE = START_ANGLE - 90;
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
export const FORECAST = [
  {key: 'tue', day: 'TUE', temps: '74° / 58°'},
  {key: 'wed', day: 'WED', temps: '71° / 55°'},
  {key: 'thu', day: 'THU', temps: '68° / 52°'},
];
