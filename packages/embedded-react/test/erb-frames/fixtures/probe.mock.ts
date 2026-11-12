// The simulators' stand-in for the host's probe module (tools/erb-frames/host.c), which native-calls.tsx,
// native-events.tsx and native-strings.tsx call.
import type {NativeMock} from 'embedded-react';
import type {Probe} from './native-probe';

let count = 0;
let stored = 0;
let refused = 0;

const probe: NativeMock<Probe> = emit => ({
  add: (left, right) => left + right,
  half: value => value / 2,
  even: value => value % 2 === 0,
  flip: on => !on,
  next: () => ++count,
  store: value => {
    stored = value;
  },
  stored: () => stored,
  start: value => {
    const post = () => emit('done', value, value / 2, value % 2 === 0);
    if (!post()) {
      refused++;
      setTimeout(post, 0);
    }
  },
  refused: () => refused,
  greet: name => `hi ${name}`,
  length: text => text.length,
  shift: (seconds, stamp) => stamp + seconds * 1000,
  announce: id => {
    setTimeout(() => emit('named', `probe-${id}`, 5000000000 + id), 0);
  },
});

export default probe;
