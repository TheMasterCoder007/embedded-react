// The simulators' stand-in for the host's probe module (tools/erb-frames/host.c), which native-calls.tsx calls.
import type {Probe} from './native-probe';

let count = 0;
let stored = 0;

const probe: Probe = {
  add: (left, right) => left + right,
  half: value => value / 2,
  even: value => value % 2 === 0,
  flip: on => !on,
  next: () => ++count,
  store: value => {
    stored = value;
  },
  stored: () => stored,
};

export default probe;
