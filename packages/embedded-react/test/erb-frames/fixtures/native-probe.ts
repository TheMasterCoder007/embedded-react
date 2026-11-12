// The host's probe module (tools/erb-frames/host.c), which native-calls.tsx, native-events.tsx,
// native-strings.tsx and native-records.tsx call.
import type {Int, Int64} from 'embedded-react';

export interface Point {
  x: Int;
  y: Int;
}

/** A string, an Int64 after an Int, so Flow B's registers have a gap, and a record inside. */
export type Fix = {
  label: string;
  id: Int;
  at: Int64;
  place: {lat: number; lon: number};
  ok: boolean;
};

export interface Probe {
  add(left: Int, right: Int): Int;
  half(value: Int): number;
  even(value: Int): boolean;
  flip(on: boolean): boolean;
  /** Counts up from 1, once per call. */
  next(): Int;
  store(value: number): void;
  stored(): number;
  /** Posts done on the next frame. It tries from inside the call first, which is refused. */
  start(value: Int): void;
  /** Posts refused so far. */
  refused(): Int;
  /** "hi " and the name. */
  greet(name: string): string;
  /** Its bytes; the fixtures' text is ASCII. */
  length(text: string): Int;
  /** The stamp plus the seconds, in milliseconds. */
  shift(seconds: Int, stamp: Int64): Int64;
  /** Posts named on the next frame: "probe-<id>", and 5000000000 plus the id. */
  announce(id: Int): void;
  /** The point, step to the right and twice the step down. */
  move(from: Point, step: Int): Point;
  /** "fix-<id>", the id, 6000000000 plus it, half of it and minus a quarter, and whether it is even. */
  fix(id: Int): Fix;
  /** Posts located on the next frame: fix(id), and the point (id, 2 * id). */
  locate(id: Int): void;
  events: {
    done(value: Int, half: number, even: boolean): void;
    /** Posted every frame; nothing subscribes. */
    idle(): void;
    named(name: string, stamp: Int64): void;
    located(where: Fix, from: Point): void;
  };
}
