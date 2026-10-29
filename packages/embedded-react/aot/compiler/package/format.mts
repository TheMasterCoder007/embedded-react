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

/*
 * The Flow B package format, as the compiler writes it: the codes and sizes in runtime/erb_format.h, the
 * engine ABI's prop ids in runtime/erb_props.c, and the er_scene.h enum values constant props name. The
 * npm package doesn't ship the C headers, so they are copied here; package-format.unit.test.js reads the
 * headers and fails when the two drift apart.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Versions and container
 ---------------------------------------------------------------------------------------------------------------------*/

export const FORMAT_MAJOR = 1;
export const FORMAT_MINOR = 5;

/** The engine ABI level the package is built against (er_scene.h's ER_ABI). */
export const ENGINE_ABI = 1;

export const PACKAGE_TAG = 'erb';

export const SECTION_PROGRAM = 4;
export const SECTION_DEBUG = 5;

export const FLAG_DEBUG = 0x0001;

/** "None", for any u16 index. */
export const NONE = 0xffff;

/** Each table of the debug section starts with its header; then its entries. */
export const DEBUG_HEADER_SIZE = 20;
export const DEBUG_ENTRY_SIZE = 16;

/*----------------------------------------------------------------------------------------------------------------------
 - Program section
 ---------------------------------------------------------------------------------------------------------------------*/

export const HEADER_SIZE = 24;
export const PANEL_SIZE = 16;
export const NEED_SIZE = 8;
export const DIRECTORY_SIZE = 12;

export const REGION_SIZE = 12;
export const SLOT_SIZE = 16;
export const SET_SIZE = 4;
export const ROW_SIZE = 8;
export const FIELD_SIZE = 8;
export const PROP_SIZE = 8;
export const TEMPLATE_SIZE = 8;
export const NODE_SIZE = 12;
export const BINDING_SIZE = 4;
export const FUNCTION_SIZE = 16;
export const ANIM_SIZE = 40;
export const INTERP_SIZE = 72;
export const TAPE_SIZE = 16;
export const TAPE_HEADER_SIZE = 16;
export const PAINT_SIZE = 24;
export const GRADIENT_SIZE = 24;
export const STOP_SIZE = 8;
export const KEYBOARD_HEADER_SIZE = 28;
export const KEYBOARD_GROUP_SIZE = 4;
export const KEYBOARD_KEY_SIZE = 8;

/** Engine resources a package names in its needs, besides nodes. */
export const Resource = {
  vectorNodes: 1,
  animValues: 2,
  animations: 3,
  animGroups: 4,
  images: 5,
  fonts: 6,
  glyphs: 7,
  extraGlyphs: 8,
} as const;

/** Table ids. */
export const Table = {
  strings: 1,
  const32: 2,
  const64: 3,
  arena: 4,
  slots: 5,
  enums: 6,
  rows: 7,
  props: 8,
  templates: 9,
  bindings: 10,
  functions: 11,
  types: 12,
  code: 13,
  anims: 14,
  tapes: 15,
  imports: 16,
  inputs: 17,
  interps: 18,
  keyboard: 19,
} as const;

/** Arena region kinds, written in this order. */
export const Region = {
  slots: 1,
  dirty: 2,
  strings: 3,
  lists: 4,
  handles: 5,
  timers: 6,
  anims: 7,
  stack: 8,
  tape: 9,
  events: 10,
} as const;

/** Cells each VM frame keeps besides its registers. */
export const FRAME_CELLS = 3;

/** Bytes a timer takes in the timer table, a running animation in the anims region, and an event in the queue. */
export const TIMER_SIZE = 16;
export const ANIM_ENTRY_SIZE = 20;
export const EVENT_SIZE = 32;

/** Bytes a list pool keeps before its rows: the row count. */
export const LIST_HEADER_SIZE = 4;

/** Register and slot value types. A 64-bit type, or a node handle, takes two register cells, the first even. */
export const ValueType = {
  pair: 0,
  int: 1,
  bool: 2,
  float: 3,
  int64: 4,
  double: 5,
  string: 6,
  enum: 7,
  node: 8,
  anim: 9,
  avalue: 10,
  list: 11,
} as const;

/** Slot flag bits. */
export const SlotFlag = {
  tracked: 0x01,
  bootOnly: 0x02,
} as const;

/** Binding kinds. */
export const BindingKind = {
  event: 1,
  query: 2,
  animated: 3,
  tape: 4,
  interpolated: 5,
} as const;

/** Animation descriptor kinds. */
export const AnimKind = {
  timing: 1,
  spring: 2,
  decay: 3,
  sequence: 4,
  parallel: 5,
  stagger: 6,
  loop: 7,
} as const;

/** Animation descriptor flag bits. */
export const AnimFlag = {
  loop: 0x01,
  loopReverse: 0x02,
  toRegister: 0x04,
} as const;

/** The paint fields VPAINT sets. */
export const PaintField = {
  fill: 0,
  stroke: 1,
  strokeWidth: 2,
  fillGradient: 3,
  strokeGradient: 4,
} as const;

/** The gradient fields VGRAD sets: the geometry, then stop k's color at `stops + 2k` and its position after it. */
export const GradientField = {
  ax: 0,
  ay: 1,
  bx: 2,
  by: 3,
  radius: 4,
  stops: 8,
} as const;

/** Interpolation flag bits. */
export const InterpFlag = {
  color: 0x01,
} as const;

/** Each ERAnimProp an animated value drives, as the ERProps field its ABI prop is. */
export const ANIM_PROP_FIELDS: ReadonlyMap<string, string> = new Map([
  ['ER_PROP_BACKGROUND_COLOR', 'background_color'],
  ['ER_PROP_OPACITY', 'opacity'],
  ['ER_PROP_COLOR', 'color'],
  ['ER_PROP_TRANSLATE_X', 'transform_translate_x'],
  ['ER_PROP_TRANSLATE_Y', 'transform_translate_y'],
  ['ER_PROP_SCALE_X', 'transform_scale_x'],
  ['ER_PROP_SCALE_Y', 'transform_scale_y'],
  ['ER_PROP_ROTATE_Z', 'transform_rotate_z'],
  ['ER_PROP_ROTATE_X', 'transform_rotate_x'],
  ['ER_PROP_ROTATE_Y', 'transform_rotate_y'],
  ['ER_PROP_SWITCH_THUMB', 'switch_value'],
  ['ER_PROP_ARC_VALUE', 'arc_value'],
  ['ER_PROP_ARC_VALUE_START', 'arc_value_start'],
]);

/** Function kinds. Below `query`, a kind is the EREventType its handler is bound to. */
export const FunctionKind = {
  query: 0x40,
  timer: 0x80,
  effect: 0x81,
  update: 0x82,
  boot: 0x83,
  helper: 0x84,
  animDone: 0x85,
} as const;

/** Event data fields EVT reads. */
export const EventField = {
  x: 0,
  y: 1,
  dx: 2,
  dy: 3,
  vx: 4,
  vy: 5,
  scrollX: 6,
  scrollY: 7,
  layoutX: 8,
  layoutY: 9,
  layoutW: 10,
  layoutH: 11,
  value: 12,
  valueStart: 13,
  finished: 14,
  touches: 15,
} as const;

/** FMATH and DMATH functions. */
export const MathFn = {
  sin: 0,
  cos: 1,
  tan: 2,
  sqrt: 3,
  abs: 4,
  round: 5,
  floor: 6,
  ceil: 7,
  min: 8,
  max: 9,
  atan2: 10,
  pow: 11,
  mod: 12,
} as const;

/** Template kinds. */
export const TemplateKind = {
  root: 1,
  branch: 2,
  row: 3,
} as const;

/** Constant prop value kinds. */
export const PropKind = {
  i16: 1,
  u8: 2,
  u16: 3,
  enum: 4,
  bool: 5,
  color: 6,
  float: 7,
  opacity: 8,
  string: 9,
  image: 10,
} as const;

export type PropKindName = keyof typeof PropKind;

/*----------------------------------------------------------------------------------------------------------------------
 - Instructions
 ---------------------------------------------------------------------------------------------------------------------*/

/** One instruction: its name, its form, and its length in words. */
export type OpcodeInfo = [name: string, form: 'abc' | 'abx', words: 1 | 2];

/** ERB_OPCODES, in order: an opcode is its index here plus one (0 is never an instruction). */
export const OPCODES: readonly OpcodeInfo[] = [
  ['MOVE', 'abc', 1],
  ['LOADI', 'abx', 1],
  ['LOADK', 'abx', 1],
  ['LOADK64', 'abx', 1],
  ['LOADS', 'abx', 1],
  ['LOADE', 'abx', 2],
  ['GET32', 'abx', 1],
  ['GET64', 'abx', 1],
  ['GETS', 'abx', 1],
  ['SET32', 'abx', 1],
  ['SET64', 'abx', 1],
  ['SETS', 'abx', 1],
  ['GETNI', 'abc', 2],
  ['LEN', 'abx', 1],
  ['ROWGET', 'abc', 2],
  ['ROWSET', 'abc', 2],
  ['APPEND', 'abx', 1],
  ['CLEAR', 'abx', 1],
  ['POP', 'abx', 1],
  ['KEEP', 'abx', 1],
  ['SLICE', 'abc', 2],
  ['ADD', 'abc', 1],
  ['SUB', 'abc', 1],
  ['MUL', 'abc', 1],
  ['DIV', 'abc', 1],
  ['FLOORDIV', 'abc', 1],
  ['CEILDIV', 'abc', 1],
  ['ROUNDDIV', 'abc', 1],
  ['MOD', 'abc', 1],
  ['MIN', 'abc', 1],
  ['MAX', 'abc', 1],
  ['NEG', 'abc', 1],
  ['ABS', 'abc', 1],
  ['ADDL', 'abc', 1],
  ['SUBL', 'abc', 1],
  ['MULL', 'abc', 1],
  ['DIVL', 'abc', 1],
  ['FLOORDIVL', 'abc', 1],
  ['CEILDIVL', 'abc', 1],
  ['ROUNDDIVL', 'abc', 1],
  ['MODL', 'abc', 1],
  ['MINL', 'abc', 1],
  ['MAXL', 'abc', 1],
  ['NEGL', 'abc', 1],
  ['ABSL', 'abc', 1],
  ['FADD', 'abc', 1],
  ['FSUB', 'abc', 1],
  ['FMUL', 'abc', 1],
  ['FDIV', 'abc', 1],
  ['FNEG', 'abc', 1],
  ['FMATH', 'abc', 2],
  ['DADD', 'abc', 1],
  ['DSUB', 'abc', 1],
  ['DMUL', 'abc', 1],
  ['DDIV', 'abc', 1],
  ['DNEG', 'abc', 1],
  ['DMATH', 'abc', 2],
  ['ITOF', 'abc', 1],
  ['FTOI', 'abc', 1],
  ['LTOF', 'abc', 1],
  ['ITOL', 'abc', 1],
  ['LTOI', 'abc', 1],
  ['DTOI', 'abc', 1],
  ['FTOD', 'abc', 1],
  ['DTOF', 'abc', 1],
  ['ITOD', 'abc', 1],
  ['LTOD', 'abc', 1],
  ['DTOL', 'abc', 1],
  ['FTOL', 'abc', 1],
  ['EQ', 'abc', 1],
  ['LT', 'abc', 1],
  ['LE', 'abc', 1],
  ['FEQ', 'abc', 1],
  ['FLT', 'abc', 1],
  ['FLE', 'abc', 1],
  ['LEQ', 'abc', 1],
  ['LLT', 'abc', 1],
  ['LLE', 'abc', 1],
  ['DEQ', 'abc', 1],
  ['DLT', 'abc', 1],
  ['DLE', 'abc', 1],
  ['SEQ', 'abc', 1],
  ['NOT', 'abc', 1],
  ['JMP', 'abx', 1],
  ['JT', 'abx', 1],
  ['JF', 'abx', 1],
  ['JDIRTY', 'abx', 2],
  ['LOOPSET', 'abx', 1],
  ['LOOP', 'abx', 1],
  ['RET', 'abc', 1],
  ['CALL', 'abx', 1],
  ['CALLN', 'abx', 1],
  ['FMTK', 'abx', 1],
  ['FMTS', 'abc', 1],
  ['FMTI', 'abc', 1],
  ['FMTL', 'abc', 1],
  ['FMTF', 'abc', 1],
  ['FMTB', 'abc', 1],
  ['FMTE', 'abx', 1],
  ['FMTFX', 'abc', 1],
  ['FMTPAD', 'abc', 1],
  ['FMTSET', 'abx', 1],
  ['FMTTEXT', 'abc', 1],
  ['FMTSPAN', 'abc', 1],
  ['PROPS', 'abx', 1],
  ['PSETI', 'abx', 1],
  ['PSETF', 'abx', 1],
  ['PSETB', 'abx', 1],
  ['PSETS', 'abx', 1],
  ['PSETE', 'abx', 2],
  ['SPANS', 'abc', 1],
  ['COMMIT', 'abc', 1],
  ['BUILD', 'abc', 2],
  ['DESTROY', 'abc', 1],
  ['SCROLL', 'abc', 1],
  ['FOCUS', 'abc', 1],
  ['BLUR', 'abc', 1],
  ['INPUT', 'abx', 1],
  ['TSTART', 'abc', 2],
  ['TCLEAR', 'abc', 1],
  ['ASTART', 'abx', 2],
  ['ASTOP', 'abc', 1],
  ['AVSET', 'abx', 1],
  ['AVGET', 'abx', 1],
  ['VTAPE', 'abx', 1],
  ['VSET', 'abx', 1],
  ['VCOMMIT', 'abx', 1],
  ['VDIRTY', 'abc', 1],
  ['EVT', 'abc', 1],
  ['NOW', 'abc', 1],
  ['DATENOW', 'abc', 1],
  ['TRAP', 'abx', 1],
  ['EFFECT', 'abx', 1],
  ['VPAINT', 'abc', 1],
  ['VGRAD', 'abc', 1],
  ['VCOMMITP', 'abx', 1],
];

/*----------------------------------------------------------------------------------------------------------------------
 - The engine ABI's props
 ---------------------------------------------------------------------------------------------------------------------*/

/** One ABI prop: the ERProps field the compiler writes, and its value kind. */
export interface AbiProp {
  id: number;
  kind: PropKindName;
  /** An enum's largest value. */
  max?: number;
}

/** The props with ids 1 to 150, in id order, as erb_props.c lists them, with each enum's largest value. */
const PROPS_IN_ORDER: [field: string, kind: PropKindName, max?: number][] = [
  ['left', 'i16'],
  ['top', 'i16'],
  ['right', 'i16'],
  ['bottom', 'i16'],
  ['width', 'i16'],
  ['height', 'i16'],
  ['min_width', 'i16'],
  ['max_width', 'i16'],
  ['min_height', 'i16'],
  ['max_height', 'i16'],
  ['padding', 'i16'],
  ['padding_left', 'i16'],
  ['padding_top', 'i16'],
  ['padding_right', 'i16'],
  ['padding_bottom', 'i16'],
  ['margin', 'i16'],
  ['margin_left', 'i16'],
  ['margin_top', 'i16'],
  ['margin_right', 'i16'],
  ['margin_bottom', 'i16'],
  ['gap', 'i16'],
  ['row_gap', 'i16'],
  ['column_gap', 'i16'],
  ['flex_grow', 'i16'],
  ['flex_shrink', 'i16'],
  ['flex_basis', 'i16'],
  ['flex_direction', 'enum', 3],
  ['flex_wrap', 'enum', 2],
  ['align_items', 'enum', 4],
  ['align_self', 'enum', 4],
  ['align_content', 'enum', 5],
  ['justify_content', 'enum', 5],
  ['position', 'enum', 1],
  ['display', 'enum', 1],
  ['aspect_ratio', 'float'],
  ['margin_horizontal', 'i16'],
  ['margin_vertical', 'i16'],
  ['padding_horizontal', 'i16'],
  ['padding_vertical', 'i16'],
  ['flex_basis_pct', 'float'],
  ['width_pct', 'float'],
  ['height_pct', 'float'],
  ['left_pct', 'float'],
  ['top_pct', 'float'],
  ['right_pct', 'float'],
  ['bottom_pct', 'float'],
  ['background_color', 'color'],
  ['border_color', 'color'],
  ['border_width', 'i16'],
  ['border_radius', 'i16'],
  ['border_top_left_radius', 'i16'],
  ['border_top_right_radius', 'i16'],
  ['border_bottom_left_radius', 'i16'],
  ['border_bottom_right_radius', 'i16'],
  ['border_left_width', 'i16'],
  ['border_top_width', 'i16'],
  ['border_right_width', 'i16'],
  ['border_bottom_width', 'i16'],
  ['border_left_color', 'color'],
  ['border_top_color', 'color'],
  ['border_right_color', 'color'],
  ['border_bottom_color', 'color'],
  ['border_style', 'enum', 2],
  ['z_index', 'i16'],
  ['opacity', 'opacity'],
  ['overflow', 'enum', 2],
  ['pointer_events', 'enum', 3],
  ['hit_slop_left', 'i16'],
  ['hit_slop_top', 'i16'],
  ['hit_slop_right', 'i16'],
  ['hit_slop_bottom', 'i16'],
  ['long_press_ms', 'u16'],
  ['text', 'string'],
  ['font_family', 'string'],
  ['color', 'color'],
  ['font_size', 'u8'],
  ['font_weight', 'enum', 1],
  ['font_style', 'enum', 1],
  ['text_align', 'enum', 2],
  ['number_of_lines', 'u8'],
  ['ellipsize_mode', 'enum', 3],
  ['text_decoration', 'enum', 2],
  ['line_height', 'i16'],
  ['letter_spacing', 'i16'],
  ['image_name', 'image'],
  ['resize_mode', 'enum', 4],
  ['tint_color', 'color'],
  ['transform_translate_x', 'float'],
  ['transform_translate_y', 'float'],
  ['transform_scale_x', 'float'],
  ['transform_scale_y', 'float'],
  ['transform_rotate_z', 'float'],
  ['transform_origin_x', 'float'],
  ['transform_origin_y', 'float'],
  ['transform_rotate_x', 'float'],
  ['transform_rotate_y', 'float'],
  ['transform_perspective', 'float'],
  ['shadow_color', 'color'],
  ['shadow_offset_x', 'float'],
  ['shadow_offset_y', 'float'],
  ['shadow_opacity', 'float'],
  ['shadow_radius', 'u8'],
  ['elevation', 'u8'],
  ['indicator_color', 'color'],
  ['animating', 'bool'],
  ['switch_value', 'bool'],
  ['track_color_false', 'color'],
  ['track_color_true', 'color'],
  ['thumb_color', 'color'],
  ['placeholder', 'string'],
  ['placeholder_color', 'color'],
  ['cursor_color', 'color'],
  ['editable', 'bool'],
  ['secure_text_entry', 'bool'],
  ['modal_visible', 'bool'],
  ['backdrop_color', 'color'],
  ['arc_value', 'float'],
  ['arc_value_start', 'float'],
  ['arc_min_span', 'float'],
  ['arc_min', 'float'],
  ['arc_max', 'float'],
  ['arc_start_angle', 'float'],
  ['arc_sweep_angle', 'float'],
  ['arc_step', 'float'],
  ['arc_gap_angle', 'float'],
  ['arc_width', 'i16'],
  ['arc_band_width', 'i16'],
  ['arc_knob_size', 'i16'],
  ['arc_knob_border_width', 'i16'],
  ['arc_track_color', 'color'],
  ['arc_indicator_color', 'color'],
  ['arc_band_color', 'color'],
  ['arc_knob_color', 'color'],
  ['arc_knob_border_color', 'color'],
  ['arc_segments', 'u8'],
  ['arc_cap', 'enum', 1],
  ['arc_knob', 'enum', 3],
  ['arc_adjustable', 'bool'],
  ['arc_range', 'bool'],
  ['gradient_type', 'enum', 3],
  ['gradient_angle', 'float'],
  ['gradient_stop_count', 'enum', 4],
  ['gradient_stops[0].color', 'color'],
  ['gradient_stops[1].color', 'color'],
  ['gradient_stops[2].color', 'color'],
  ['gradient_stops[3].color', 'color'],
  ['gradient_stops[0].position', 'float'],
  ['gradient_stops[1].position', 'float'],
  ['gradient_stops[2].position', 'float'],
  ['gradient_stops[3].position', 'float'],
];

/** Span props start at this id, SPAN_STRIDE ids per span. */
export const SPAN_PROP_FIRST = 160;
export const SPAN_STRIDE = 8;
export const MAX_SPANS = 4;

/** A span's fields, in its ids. */
const SPAN_FIELDS: [field: string, kind: PropKindName][] = [
  ['color', 'color'],
  ['font_size', 'u8'],
  ['font_weight', 'u8'],
  ['font_style', 'u8'],
  ['text_decoration', 'u8'],
  ['letter_spacing', 'i16'],
];

/** Every ABI prop, by the ERProps field it sets. */
export const ABI_PROPS: ReadonlyMap<string, AbiProp> = new Map([
  ...PROPS_IN_ORDER.map(([field, kind, max], index): [string, AbiProp] => [
    field,
    max === undefined ? {id: index + 1, kind} : {id: index + 1, kind, max},
  ]),
  ...Array.from({length: MAX_SPANS}, (_, span) =>
    SPAN_FIELDS.map(([field, kind], fieldIndex): [string, AbiProp] => [
      `spans[${span}].${field}`,
      {id: SPAN_PROP_FIRST + span * SPAN_STRIDE + fieldIndex, kind},
    ]),
  ).flat(),
]);

/*----------------------------------------------------------------------------------------------------------------------
 - Engine values
 ---------------------------------------------------------------------------------------------------------------------*/

/** er_scene.h's ER_LAYOUT_AUTO (INT16_MIN). */
export const LAYOUT_AUTO = -32768;

/** The er_scene.h names the compiler spells constant props and node types with, and their values. */
export const ENGINE_VALUES: ReadonlyMap<string, number> = new Map([
  ['ER_LAYOUT_AUTO', LAYOUT_AUTO],
  ['ER_NODE_VIEW', 0],
  ['ER_NODE_TEXT', 1],
  ['ER_NODE_IMAGE', 2],
  ['ER_NODE_SCROLL_VIEW', 3],
  ['ER_NODE_FLAT_LIST', 4],
  ['ER_NODE_PRESSABLE', 5],
  ['ER_NODE_TEXT_INPUT', 6],
  ['ER_NODE_ACTIVITY_INDICATOR', 7],
  ['ER_NODE_SWITCH', 8],
  ['ER_NODE_MODAL', 9],
  ['ER_NODE_VECTOR', 10],
  ['ER_NODE_ARC', 11],
  ['ER_FLEX_COL', 0],
  ['ER_FLEX_ROW', 1],
  ['ER_FLEX_ROW_REVERSE', 2],
  ['ER_FLEX_COL_REVERSE', 3],
  ['ER_WRAP_NOWRAP', 0],
  ['ER_WRAP_WRAP', 1],
  ['ER_WRAP_WRAP_REVERSE', 2],
  ['ER_ALIGN_AUTO', 0],
  ['ER_ALIGN_STRETCH', 1],
  ['ER_ALIGN_FLEX_START', 2],
  ['ER_ALIGN_CENTER', 3],
  ['ER_ALIGN_FLEX_END', 4],
  ['ER_ALIGN_CONTENT_FLEX_START', 0],
  ['ER_ALIGN_CONTENT_FLEX_END', 1],
  ['ER_ALIGN_CONTENT_CENTER', 2],
  ['ER_ALIGN_CONTENT_STRETCH', 3],
  ['ER_ALIGN_CONTENT_SPACE_BETWEEN', 4],
  ['ER_ALIGN_CONTENT_SPACE_AROUND', 5],
  ['ER_JUSTIFY_FLEX_START', 0],
  ['ER_JUSTIFY_CENTER', 1],
  ['ER_JUSTIFY_FLEX_END', 2],
  ['ER_JUSTIFY_SPACE_BETWEEN', 3],
  ['ER_JUSTIFY_SPACE_AROUND', 4],
  ['ER_JUSTIFY_SPACE_EVENLY', 5],
  ['ER_POS_RELATIVE', 0],
  ['ER_POS_ABSOLUTE', 1],
  ['ER_DISPLAY_FLEX', 0],
  ['ER_DISPLAY_NONE', 1],
  ['ER_BORDER_SOLID', 0],
  ['ER_BORDER_DASHED', 1],
  ['ER_BORDER_DOTTED', 2],
  ['ER_OVERFLOW_VISIBLE', 0],
  ['ER_OVERFLOW_HIDDEN', 1],
  ['ER_OVERFLOW_SCROLL', 2],
  ['ER_POINTER_EVENTS_AUTO', 0],
  ['ER_POINTER_EVENTS_NONE', 1],
  ['ER_POINTER_EVENTS_BOX_ONLY', 2],
  ['ER_POINTER_EVENTS_BOX_NONE', 3],
  ['ER_FONT_STYLE_NORMAL', 0],
  ['ER_FONT_STYLE_ITALIC', 1],
  ['ER_TEXT_ALIGN_LEFT', 0],
  ['ER_TEXT_ALIGN_CENTER', 1],
  ['ER_TEXT_ALIGN_RIGHT', 2],
  ['ER_TEXT_ELLIPSIZE_TAIL', 0],
  ['ER_TEXT_ELLIPSIZE_HEAD', 1],
  ['ER_TEXT_ELLIPSIZE_MIDDLE', 2],
  ['ER_TEXT_ELLIPSIZE_CLIP', 3],
  ['ER_TEXT_DECORATION_NONE', 0],
  ['ER_TEXT_DECORATION_UNDERLINE', 1],
  ['ER_TEXT_DECORATION_LINE_THROUGH', 2],
  ['ER_RESIZE_COVER', 0],
  ['ER_RESIZE_CONTAIN', 1],
  ['ER_RESIZE_STRETCH', 2],
  ['ER_RESIZE_REPEAT', 3],
  ['ER_RESIZE_CENTER', 4],
  ['ER_ARC_CAP_BUTT', 0],
  ['ER_ARC_CAP_ROUND', 1],
  ['ER_ARC_KNOB_NONE', 0],
  ['ER_ARC_KNOB_CIRCLE', 1],
  ['ER_ARC_KNOB_IMAGE', 2],
  ['ER_ARC_KNOB_CHILD', 3],
  ['ER_GRADIENT_NONE', 0],
  ['ER_GRADIENT_LINEAR', 1],
  ['ER_GRADIENT_RADIAL', 2],
  ['ER_GRADIENT_CONIC', 3],
  ['ER_EVENT_PRESS', 0],
  ['ER_EVENT_LONG_PRESS', 1],
  ['ER_EVENT_PRESS_IN', 2],
  ['ER_EVENT_PRESS_OUT', 3],
  ['ER_EVENT_TOUCH_START', 4],
  ['ER_EVENT_TOUCH_MOVE', 5],
  ['ER_EVENT_TOUCH_END', 6],
  ['ER_EVENT_TOUCH_CANCEL', 7],
  ['ER_EVENT_SCROLL', 8],
  ['ER_EVENT_RESPONDER_GRANT', 9],
  ['ER_EVENT_RESPONDER_REJECT', 10],
  ['ER_EVENT_RESPONDER_MOVE', 11],
  ['ER_EVENT_RESPONDER_RELEASE', 12],
  ['ER_EVENT_RESPONDER_TERMINATE', 13],
  ['ER_EVENT_LAYOUT', 14],
  ['ER_EVENT_CHANGE_TEXT', 15],
  ['ER_EVENT_SUBMIT_EDITING', 16],
  ['ER_EVENT_FOCUS', 17],
  ['ER_EVENT_BLUR', 18],
  ['ER_EVENT_VALUE_CHANGE', 19],
  ['ER_QUERY_START_SHOULD_SET', 0],
  ['ER_QUERY_START_SHOULD_SET_CAPTURE', 1],
  ['ER_QUERY_MOVE_SHOULD_SET', 2],
  ['ER_QUERY_MOVE_SHOULD_SET_CAPTURE', 3],
  ['ER_QUERY_TERMINATION_REQUEST', 4],
  ['ER_EASE_LINEAR', 0],
  ['ER_EASE_EASE', 1],
  ['ER_EASE_EASE_IN', 2],
  ['ER_EASE_EASE_OUT', 3],
  ['ER_EASE_EASE_IN_OUT', 4],
  ['ER_EASE_QUAD_IN', 5],
  ['ER_EASE_QUAD_OUT', 6],
  ['ER_EASE_QUAD_IN_OUT', 7],
  ['ER_EASE_CUBIC_IN', 8],
  ['ER_EASE_CUBIC_OUT', 9],
  ['ER_EASE_CUBIC_IN_OUT', 10],
  ['ER_EASE_BOUNCE_OUT', 11],
  ['ER_EASE_ELASTIC_OUT', 12],
  ['ER_EASE_BEZIER', 13],
  ['ER_EXTRAPOLATE_EXTEND', 0],
  ['ER_EXTRAPOLATE_CLAMP', 1],
  ['ER_EXTRAPOLATE_IDENTITY', 2],
  ['ER_KBD_KEY_CHAR', 0],
  ['ER_KBD_KEY_BACKSPACE', 1],
  ['ER_KBD_KEY_DONE', 2],
  ['ER_KBD_KEY_LAYER', 3],
]);
