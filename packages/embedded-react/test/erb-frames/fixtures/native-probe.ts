// The host's probe module (tools/erb-frames/host.c), which native-calls.tsx, native-events.tsx and
// native-strings.tsx call.
import type {Int, Int64} from 'embedded-react';

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
  events: {
    done(value: Int, half: number, even: boolean): void;
    /** Posted every frame; nothing subscribes. */
    idle(): void;
    named(name: string, stamp: Int64): void;
  };
}
