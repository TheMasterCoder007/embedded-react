// Runtime e2e: React keyed-list reordering must drive the engine's node moves (insertBefore /
// appendChild) correctly. Renders a keyed list, re-renders it reversed, and reads each item's
// computed y via onLayout to assert the order reversed. Runs in the QuickJS + engine host.
import { createRoot } from '../../src/renderer.js';
import { View } from '../../src/components.js';
import { check, report } from './harness.js';

const ys = {};
let listRect = null;

function List({ order }) {
  return (
    <View style={{ width: 100, height: 200 }} onLayout={(e) => (listRect = e.layout)}>
      {order.map((i) => (
        <View key={i} style={{ width: 40, height: 20 }} onLayout={(e) => (ys[i] = e.layout.y)} />
      ))}
    </View>
  );
}

const root = createRoot({ width: screen.width, height: screen.height });

root.render(<List order={[0, 1, 2]} />);
const mount = { 0: ys[0], 1: ys[1], 2: ys[2] };

// Sanity: the fixed-size list keeps its height and items stack at their own heights (regression
// for the flex-default bug where un-flexed nodes grew to fill their parent).
check(listRect && listRect.height === 200, 'list keeps explicit height 200');
check(mount[0] === 0 && mount[1] === 20 && mount[2] === 40, 'items stack at 0,20,40 on mount');

root.render(<List order={[2, 1, 0]} />);

// After reverse: key2 takes the first slot, key0 the last, key1 stays put.
check(ys[2] === mount[0], 'key 2 moved to the first slot');
check(ys[1] === mount[1], 'key 1 stayed in the middle');
check(ys[0] === mount[2], 'key 0 moved to the last slot');

report('reorder');
