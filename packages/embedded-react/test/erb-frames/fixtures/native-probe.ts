// The host's probe module (tools/erb-frames/host.c), which native-calls.tsx calls.
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
}
