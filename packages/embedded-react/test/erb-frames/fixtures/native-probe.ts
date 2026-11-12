// The host's probe module (tools/erb-frames/host.c), which native-calls.tsx and native-events.tsx call.
import type {Int} from 'embedded-react';

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
  events: {
    done(value: Int, half: number, even: boolean): void;
    /** Posted every frame; nothing subscribes. */
    idle(): void;
  };
}
