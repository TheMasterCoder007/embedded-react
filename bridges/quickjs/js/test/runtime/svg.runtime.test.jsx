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

// Runtime e2e: an <Svg> mounts as ONE vector node (its Path/Circle children are flattened, not
// mounted), lays out at its style size, and re-renders with new geometry without crashing. The
// headless harness uses a no-op backend, so this still drives the full path: flattenSvg ->
// NativeUI.setVectorOps -> er_node_set_vector_ops -> er_vector_render (the rasterizer runs).
// Pixels aren't observable from JS, so we assert layout + no-crash across renders.
import {useRef} from 'react';
import {createRoot} from '../../src/renderer.js';
import {View, Svg, Path, Circle, updateVector} from 'embedded-react';
import {NativeUI} from '../../src/native-ui.js';
import {check, report} from './harness.js';

const layouts = {};

function Dial({value}) {
  const ex = 100 + value;
  const ey = 100 - value;
  return (
    <View style={{width: 200, height: 200}}>
      <Svg
        style={{position: 'absolute', left: 0, top: 0, width: 200, height: 200}}
        onLayout={e => (layouts.svg = e.layout)}>
        <Path
          d="M 20 180 A 80 80 0 1 1 180 180"
          stroke="#2c3a4f"
          strokeWidth={10}
          strokeLinecap="round"
          fill="none"
        />
        {value > 0 && (
          <Path
            d={`M 20 180 A 80 80 0 0 1 ${ex} ${ey}`}
            stroke="#f4a261"
            strokeWidth={10}
            strokeLinecap="round"
            fill="none"
          />
        )}
        <Circle
          cx={ex}
          cy={ey}
          r={8}
          fill="#16202f"
          stroke="#f4a261"
          strokeWidth={3}
        />
      </Svg>
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});

root.render(<Dial value={10} />);
check(layouts.svg != null, 'Svg node rendered (onLayout fired)');
check(
  layouts.svg && layouts.svg.width === 200 && layouts.svg.height === 200,
  'Svg laid out at its style size (200x200)',
);

// Re-render with new geometry — the op-tape changes and the node re-rasterizes; must not crash.
root.render(<Dial value={40} />);
check(true, 're-render with new path geometry did not crash');

// Many rapid geometry updates (simulates dragging) — exercises store/free slot reuse.
for (let v = 0; v <= 60; v += 5) root.render(<Dial value={v} />);
check(true, 'rapid geometry updates (drag simulation) did not crash');

// An empty <Svg> (no shapes) must clear cleanly.
root.render(
  <View style={{width: 200, height: 200}}>
    <Svg style={{width: 200, height: 200}} />
  </View>,
);
check(true, 'empty Svg renders without crash');

// Miter + bevel joins on sharp (90°) corners, and an even-odd fill with a hole — these exercise the
// stroke_subpath join branches and the even-odd winding rule that the dial's round-only geometry never
// reaches. Pixels aren't observable headless, so this is a no-crash guard over those rasterizer paths.
root.render(
  <View style={{width: 200, height: 200}}>
    <Svg style={{width: 200, height: 200}}>
      <Path
        d="M20 20 L100 20 L100 90"
        stroke="#4cc9f0"
        strokeWidth={12}
        strokeLinejoin="miter"
        fill="none"
      />
      <Path
        d="M20 110 L100 110 L100 180"
        stroke="#2a9d8f"
        strokeWidth={12}
        strokeLinejoin="bevel"
        fill="none"
      />
      <Path
        d="M120 20 L180 20 L180 80 L120 80 Z M140 40 L160 40 L160 60 L140 60 Z"
        fill="#f4a261"
        fillRule="evenodd"
      />
    </Svg>
  </View>,
);
check(true, 'miter + bevel joins and even-odd fill rasterized without crash');

// The imperative drag path: grab the Svg's node handle via a ref, then push geometry + a node-local
// dirty rect through updateVector()/NativeUI.commit() (bypassing React, like the thermostat dial does).
let imperativeRef = null;
function ImperativeSvg() {
  const ref = useRef(null);
  imperativeRef = ref;
  return (
    <View style={{width: 200, height: 200}}>
      <Svg
        ref={ref}
        style={{position: 'absolute', left: 0, top: 0, width: 200, height: 200}}
      />
    </View>
  );
}
root.render(<ImperativeSvg />);
check(imperativeRef.current != null, 'Svg ref resolved to a node handle');

// Sweep the progress arc imperatively with a tight dirty rect each step — slot reuse + sub-region damage.
for (let a = -135; a <= 135; a += 30) {
  updateVector(
    imperativeRef.current,
    [
      {
        arc: [100, 100, 80, -135, 135],
        stroke: '#2c3a4f',
        strokeWidth: 10,
        cap: 'round',
      },
      {
        arc: [100, 100, 80, -135, a],
        stroke: '#f4a261',
        strokeWidth: 10,
        cap: 'round',
      },
      {
        circle: [100, 20, 8],
        fill: '#16202f',
        stroke: '#f4a261',
        strokeWidth: 3,
      },
    ],
    [0, 0, 200, 200],
  );
  NativeUI.commit();
}
check(
  true,
  'imperative updateVector drag (geometry + dirtyRect + commit) did not crash',
);

// <Svg source> — an imported .svg's baked artifact ({kind:'vector', ops, paints, width, height}) renders
// and scales from its intrinsic size to the node's box (the path the .svg loader + scaleVectorArtifact feed).
const importedIcon = {
  kind: 'vector',
  ops: [0, 0, 1, 10, 10, 2, 90, 90],
  paints: [0xfff4a261, 0, 4, 4, 0, 0, 0],
  width: 100,
  height: 100,
};
root.render(
  <View style={{width: 50, height: 50}}>
    <Svg
      source={importedIcon}
      style={{position: 'absolute', left: 0, top: 0, width: 50, height: 50}}
      onLayout={e => (layouts.src = e.layout)}
    />
  </View>,
);
check(
  layouts.src && layouts.src.width === 50 && layouts.src.height === 50,
  '<Svg source> mounted + laid out at its box (50x50)',
);
// re-render with a different box — re-scales, must not crash
root.render(
  <View style={{width: 120, height: 120}}>
    <Svg source={importedIcon} style={{width: 120, height: 120}} />
  </View>,
);
check(true, '<Svg source> re-scaled to a new box did not crash');

// <Svg source> RASTER fallback (Track C): an imported .svg that used unsupported features was rasterized at
// build time into a {kind:'raster', name, width, height} artifact. host-config renders it as an IMAGE node
// (not a vector node), sized to the box. The asset name need not resolve to pixels here (no-op backend); we
// assert it mounts + lays out at its box without crashing, proving the Svg→Image routing.
const rasterIcon = {
  kind: 'raster',
  name: 'baked_icon',
  width: 100,
  height: 100,
};
root.render(
  <View style={{width: 64, height: 64}}>
    <Svg
      source={rasterIcon}
      style={{position: 'absolute', left: 0, top: 0, width: 64, height: 64}}
      onLayout={e => (layouts.raster = e.layout)}
    />
  </View>,
);
check(
  layouts.raster && layouts.raster.width === 64 && layouts.raster.height === 64,
  '<Svg source> raster fallback mounted as an image node and laid out at its box (64x64)',
);
// re-render at a new box — the raster Svg updates as an image, must not crash
root.render(
  <View style={{width: 100, height: 100}}>
    <Svg source={rasterIcon} style={{width: 100, height: 100}} />
  </View>,
);
check(true, '<Svg source> raster fallback re-sized did not crash');

report('svg');
