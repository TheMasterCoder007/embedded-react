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

// @runtime-intrinsics: typed-arrays
//
// NativeUI.setVectorOps takes the op-tape either as a plain Array or, when the build has typed
// arrays, as a Float32Array it can read in one memcpy. This pins the two together: the same numbers
// through either path have to paint the same pixels. __pixel() reads the framebuffer back, so a fast
// path that mis-read length, byte offset or stride would show up as ink in the wrong place.
import {useRef} from 'react';
import {createRoot} from '../../src/renderer.js';
import {View, Svg} from 'embedded-react';
import {NativeUI} from '../../src/native-ui.js';
import {check, report} from './harness.js';

const VOP_SHAPE = 0;
const VOP_MOVE = 1;
const VOP_LINE = 2;
const VOP_CLOSE = 5;
const FILL = 0xfff4a261;
const PAINTS = [FILL, 0, 0, 4, 0, 0, 0, 0, 0]; // opaque fill, no stroke

/** A filled 40x40 right triangle with its square corner at (x, y). */
function tapeAt(x, y) {
  return [
    VOP_SHAPE,
    0,
    VOP_MOVE,
    x,
    y,
    VOP_LINE,
    x + 40,
    y,
    VOP_LINE,
    x + 40,
    y + 40,
    VOP_CLOSE,
  ];
}

/** A point well inside the triangle tapeAt(x, y) draws. */
const inside = (x, y) => [x + 30, y + 20];

let svgRef = null;
function Canvas() {
  const ref = useRef(null);
  svgRef = ref;
  return (
    <View style={{width: 200, height: 200, backgroundColor: '#101820'}}>
      <Svg
        ref={ref}
        style={{position: 'absolute', left: 0, top: 0, width: 200, height: 200}}
      />
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});
root.render(<Canvas />);
check(svgRef.current != null, 'Svg ref resolved to a node handle');
check(
  typeof Float32Array !== 'undefined',
  'harness ran with the typed-array intrinsic',
);

/** Uploads a tape, commits, and reports whether the triangle at (x, y) is filled. */
function paintedAt(ops, x, y) {
  NativeUI.setVectorOps(svgRef.current, ops, PAINTS);
  NativeUI.commit();
  const p = inside(x, y);
  return __pixel(p[0], p[1]) === FILL;
}

const A = [10, 10];
const B = [140, 140];

// Baseline: a plain-Array tape fills where it says it will, and nowhere else.
check(paintedAt(tapeAt(...A), ...A), 'plain Array tape filled its triangle');
check(!paintedAt(tapeAt(...A), ...B), 'and left the far corner unpainted');

// The same numbers through the typed-array path have to land in the same place.
check(
  paintedAt(Float32Array.from(tapeAt(...B)), ...B),
  'Float32Array tape filled the same triangle',
);
check(
  !paintedAt(Float32Array.from(tapeAt(...B)), ...A),
  'and left the far corner unpainted',
);

// A Float32Array view into a larger buffer: the fast path has to honour its byte offset and length,
// not the whole buffer's. Padding either side is nonzero, so reading past the view paints garbage.
const tape = tapeAt(...A);
const backing = new Float32Array(tape.length + 8).fill(99);
backing.set(tape, 4);
check(
  paintedAt(backing.subarray(4, 4 + tape.length), ...A),
  'an offset Float32Array view filled the right triangle',
);
check(
  !paintedAt(backing.subarray(4, 4 + tape.length), ...B),
  'and did not spill into the far corner',
);

// An empty typed array clears the geometry, exactly as an empty Array does.
check(
  !paintedAt(new Float32Array(0), ...A),
  'an empty Float32Array cleared the geometry',
);

report('vector-typed-tape');
