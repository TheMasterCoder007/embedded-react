// Runtime e2e: an <Svg> mounts as ONE vector node (its Path/Circle children are flattened, not
// mounted), lays out at its style size, and re-renders with new geometry without crashing. The
// headless harness uses a no-op backend, so this still drives the full path: flattenSvg ->
// NativeUI.setVectorOps -> er_node_set_vector_ops -> er_vector_render (the rasterizer runs).
// Pixels aren't observable from JS, so we assert layout + no-crash across renders.
import { createRoot } from '../../src/renderer.js';
import { View, Svg, Path, Circle } from 'embedded-react';
import { check, report } from './harness.js';

const layouts = {};

function Dial({ value }) {
  const ex = 100 + value;
  const ey = 100 - value;
  return (
    <View style={{ width: 200, height: 200 }}>
      <Svg
        style={{ position: 'absolute', left: 0, top: 0, width: 200, height: 200 }}
        onLayout={(e) => (layouts.svg = e.layout)}
      >
        <Path d="M 20 180 A 80 80 0 1 1 180 180" stroke="#2c3a4f" strokeWidth={10} strokeLinecap="round" fill="none" />
        {value > 0 && (
          <Path d={`M 20 180 A 80 80 0 0 1 ${ex} ${ey}`} stroke="#f4a261" strokeWidth={10} strokeLinecap="round" fill="none" />
        )}
        <Circle cx={ex} cy={ey} r={8} fill="#16202f" stroke="#f4a261" strokeWidth={3} />
      </Svg>
    </View>
  );
}

const root = createRoot({ width: screen.width, height: screen.height });

root.render(<Dial value={10} />);
check(layouts.svg != null, 'Svg node rendered (onLayout fired)');
check(
  layouts.svg && layouts.svg.width === 200 && layouts.svg.height === 200,
  'Svg laid out at its style size (200x200)'
);

// Re-render with new geometry — the op-tape changes and the node re-rasterizes; must not crash.
root.render(<Dial value={40} />);
check(true, 're-render with new path geometry did not crash');

// Many rapid geometry updates (simulates dragging) — exercises store/free slot reuse.
for (let v = 0; v <= 60; v += 5) root.render(<Dial value={v} />);
check(true, 'rapid geometry updates (drag simulation) did not crash');

// An empty <Svg> (no shapes) must clear cleanly.
root.render(
  <View style={{ width: 200, height: 200 }}>
    <Svg style={{ width: 200, height: 200 }} />
  </View>
);
check(true, 'empty Svg renders without crash');

report();
