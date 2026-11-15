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

// React Native's `backgroundImage` style, read into the one gradient the engine draws behind a View. Both
// flows read it here: Flow A hands the result to the bridge, Flow B lowers it to ERProps.

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A gradient as the engine takes it. */
export interface BackgroundGradient {
  type: 'linear' | 'radial';
  /** Engine degrees: 0 runs top to bottom, 90 left to right. */
  angle: number;
  /** ERGradientCorner: 0 for none, then top right, bottom right, bottom left, top left. */
  corner: number;
  /** Two to four stops, ascending, each offset 0 to 1. */
  stops: {color: string; offset: number}[];
}

/** A gradient, or why the value is not one the engine draws. */
export type BackgroundImageResult =
  | {gradient: BackgroundGradient; problem?: undefined}
  | {gradient?: undefined; problem: string};

/** A stop before its offset is settled. */
interface PendingStop {
  color: string;
  offset: number | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The engine's ER_GRADIENT_MAX_STOPS. */
export const BACKGROUND_MAX_STOPS = 4;

/** CSS degrees of each side: 0 points to the top, 90 to the right. */
const SIDE_ANGLES: Record<string, number> = {
  top: 0,
  right: 90,
  bottom: 180,
  left: 270,
};

/** ERGradientCorner of each corner, by its vertical then horizontal side. */
const CORNERS: Record<string, number> = {
  'top right': 1,
  'bottom right': 2,
  'bottom left': 3,
  'top left': 4,
};

/** Degrees in one of each CSS angle unit. */
const ANGLE_UNITS: Record<string, number> = {
  deg: 1,
  grad: 0.9,
  rad: 180 / Math.PI,
  turn: 360,
};

/** The characters a number starts with. */
const NUMBER_START = '0123456789.+-';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

// The parser uses no regular expressions: each costs bytecode in every Flow A bundle, and a case-insensitive
// one costs kilobytes. A problem is thrown as its message.

/**
 * Refuses a value the engine does not draw.
 *
 * @param message  What is wrong with it.
 */
function refuse(message: string): never {
  throw message;
}

/**
 * Splits text at a separator, leaving what is inside parentheses whole.
 *
 * @param text  The text.
 * @param separator  `,` or a space, which stands for any run of whitespace.
 *
 * @returns The parts, trimmed, with no empty ones.
 */
function splitOutside(text: string, separator: ',' | ' '): string[] {
  const parts: string[] = [];
  let depth = 0;
  let part = '';
  // By index: a for...of over a string never ends on the ESP32's QuickJS.
  for (let index = 0; index < text.length; index++) {
    const character = text[index];
    if (character === '(') depth++;
    if (character === ')') depth--;
    // Whitespace sorts at or below the space.
    if (
      depth === 0 &&
      (separator === ',' ? character === ',' : character <= ' ')
    ) {
      parts.push(part);
      part = '';
    } else {
      part += character;
    }
  }
  parts.push(part);
  return parts.map(entry => entry.trim()).filter(entry => entry !== '');
}

/**
 * A number with a unit after it.
 *
 * @param text  The text.
 * @param unit  The unit it must end with.
 *
 * @returns The number, or NaN when the text is not one.
 */
function numberWith(text: string, unit: string): number {
  const digits = text.slice(0, -unit.length);
  return text.endsWith(unit) && digits !== '' ? Number(digits) : NaN;
}

/**
 * A `direction`: an angle, or `to` a side or corner.
 *
 * @param direction  The direction, lowercase.
 *
 * @returns The engine's angle and corner, or null when it is not a direction.
 */
function parseDirection(
  direction: string,
): {angle: number; corner: number} | null {
  let cssDegrees = NaN;
  if (direction.startsWith('to ')) {
    const sides = splitOutside(direction.slice(3), ' ');
    // Either order names the corner.
    const corner =
      CORNERS[`${sides[0]} ${sides[1]}`] ?? CORNERS[`${sides[1]} ${sides[0]}`];
    if (sides.length === 2 && corner) return {angle: 0, corner};
    if (sides.length !== 1 || !Object.hasOwn(SIDE_ANGLES, sides[0])) {
      refuse(`"${direction}" is not a direction`);
    }
    cssDegrees = SIDE_ANGLES[sides[0]];
  } else {
    // `grad` is tried before `rad`, which it ends with.
    for (const unit in ANGLE_UNITS) {
      if (Number.isNaN(cssDegrees)) {
        cssDegrees = numberWith(direction, unit) * ANGLE_UNITS[unit];
      }
    }
    if (Number.isNaN(cssDegrees)) return null;
  }

  // CSS 0 points to the top and the engine's 0 to the bottom; both turn clockwise.
  return {angle: (((180 - cssDegrees) % 360) + 360) % 360, corner: 0};
}

/**
 * A stop's position.
 *
 * @param position  A percentage.
 *
 * @returns It as 0 to 1.
 */
function parsePosition(position: unknown): number {
  const offset =
    typeof position === 'string' ? numberWith(position.trim(), '%') / 100 : NaN;
  if (Number.isNaN(offset)) {
    refuse(
      `a stop position must be a percentage, got ${JSON.stringify(position)}`,
    );
  }
  if (offset < 0 || offset > 1) {
    refuse(`a stop position must be 0% to 100%, got ${position}`);
  }

  return offset;
}

/**
 * Settles every stop's offset as CSS does: the ends default to 0 and 1, a stop without one sits evenly between
 * its neighbours, and one below an earlier stop is raised to it.
 *
 * @param pending  The stops as written.
 *
 * @returns The stops.
 */
function settleStops(pending: PendingStop[]): BackgroundGradient['stops'] {
  if (pending.length < 2 || pending.length > BACKGROUND_MAX_STOPS) {
    refuse(
      `a gradient takes 2 to ${BACKGROUND_MAX_STOPS} stops, got ${pending.length}`,
    );
  }

  const offsets = pending.map(stop => stop.offset);
  offsets[0] ??= 0;
  offsets[offsets.length - 1] ??= 1;
  let highest = 0;
  for (let index = 0; index < offsets.length; index++) {
    if (offsets[index] !== null) {
      highest = offsets[index] = Math.max(offsets[index]!, highest);
    }
  }
  for (let index = 1; index < offsets.length; index++) {
    if (offsets[index] !== null) continue;

    let next = index;
    while (offsets[next] === null) next++;
    const before = offsets[index - 1]!;
    const step = (offsets[next]! - before) / (next - index + 1);
    for (let fill = index; fill < next; fill++) {
      offsets[fill] = before + step * (fill - index + 1);
    }
  }

  return pending.map((stop, index) => ({
    color: stop.color,
    offset: offsets[index]!,
  }));
}

/**
 * Checks a radial gradient is the one the engine draws: a circle to the farthest corner, from the centre.
 *
 * @param shape  `circle`, or with a size and position after it, lowercase.
 */
function checkRadial(shape: string): void {
  const rest = splitOutside(shape, ' ')
    .filter(word => word !== 'farthest-corner')
    .join(' ');
  if (
    ![
      'circle',
      'circle at center',
      'circle at center center',
      'circle at 50% 50%',
    ].includes(rest)
  ) {
    refuse(
      'a radial gradient must be `circle`, to the farthest corner, at the center',
    );
  }
}

/**
 * A CSS gradient function.
 *
 * @param text  `linear-gradient(...)` or `radial-gradient(...)`.
 *
 * @returns The gradient.
 */
function parseString(text: string): BackgroundGradient {
  // Colors and keywords read the same in any case.
  const lower = text.trim().toLowerCase();
  const type = lower.startsWith('linear') ? 'linear' : 'radial';
  if (splitOutside(lower, ',').length > 1) refuse('a View takes one gradient');
  if (!lower.startsWith(`${type}-gradient(`) || !lower.endsWith(')')) {
    refuse('expected linear-gradient(...) or radial-gradient(...)');
  }

  const parts = splitOutside(lower.slice(type.length + 10, -1), ',');
  let direction = {angle: 0, corner: 0};
  const first = parts[0] ?? '';
  if (type === 'linear') {
    const parsed = parseDirection(first);
    if (parsed) {
      direction = parsed;
      parts.shift();
    }
  } else if (
    ['ci', 'el', 'cl', 'fa', 'at'].includes(first.slice(0, 2)) ||
    NUMBER_START.includes(first[0])
  ) {
    // A shape, a size or a position comes before the stops.
    checkRadial(first);
    parts.shift();
  } else {
    checkRadial('');
  }

  const pending: PendingStop[] = [];
  for (const part of parts) {
    const words = splitOutside(part, ' ');
    const positions: number[] = [];
    while (
      words.length > 1 &&
      NUMBER_START.includes(words[words.length - 1][0])
    ) {
      positions.unshift(parsePosition(words.pop()));
    }
    if (
      words.length !== 1 ||
      positions.length > 2 ||
      NUMBER_START.includes(words[0][0])
    ) {
      refuse(`"${part}" is not a color stop`);
    }
    for (const offset of positions.length ? positions : [null]) {
      pending.push({color: words[0], offset});
    }
  }

  return {type, ...direction, stops: settleStops(pending)};
}

/**
 * React Native's object form of a gradient, as the CSS function it stands for.
 *
 * @param value  `{type, direction?, colorStops}` or `{type, shape, size?, position?, colorStops}`.
 *
 * @returns The function, for parseString.
 */
function objectToString(value: Record<string, unknown>): string {
  if (!Array.isArray(value.colorStops)) {
    refuse('a gradient needs `colorStops`');
  }

  const position = value.position;
  const centered =
    typeof position === 'object' &&
    position !== null &&
    Object.values(position).every(side => side === '50%');
  const before =
    value.type === 'radial-gradient'
      ? [
          value.shape,
          value.size,
          position === undefined ? '' : `at ${centered ? 'center' : position}`,
        ].join(' ')
      : (value.direction ?? '');
  const stops = (value.colorStops as Record<string, unknown>[]).map(stop => {
    if (typeof stop?.color !== 'string') {
      refuse('each stop needs a `color` string');
    }

    return [stop.color].concat((stop.positions ?? []) as string[]).join(' ');
  });
  return `${value.type}(${[before, ...stops].join(',')})`;
}

/**
 * Reads a `backgroundImage` style.
 *
 * The engine draws one gradient of two to four stops behind a View: linear at any angle or to a side or
 * corner, or a radial circle from the centre to the farthest corner.
 *
 * @param value  A CSS `linear-gradient()` or `radial-gradient()` string, or React Native's array of one
 *   gradient object.
 *
 * @returns The gradient, or the problem with the value.
 */
export function parseBackgroundImage(value: unknown): BackgroundImageResult {
  try {
    if (typeof value === 'string') {
      return {gradient: parseString(value)};
    }

    const layers = Array.isArray(value) ? value : [value];
    if (layers.length !== 1) {
      refuse('a View takes one gradient');
    }
    if (typeof layers[0] !== 'object' || layers[0] === null) {
      refuse('expected a gradient string, or an array of one object');
    }

    return {gradient: parseString(objectToString(layers[0]))};
  } catch (problem) {
    if (typeof problem !== 'string') throw problem;

    return {problem};
  }
}
