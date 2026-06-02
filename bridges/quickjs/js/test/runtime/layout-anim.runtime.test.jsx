// Runtime e2e: LayoutAnimation.configureNext() must arm the engine so the next commit tweens nodes
// whose computed rect moved. The tween itself runs in C against node->animated (display rect) and is
// covered by the engine's test_layout_anim.c. Here we verify the bridge integration:
// NativeUI.hasPendingLayoutAnimation() reports a config armed for the *next* commit — true right
// after configureNext(), then consumed (cleared) by the commit — and the final computed layout is
// correct. We also confirm a layout change with no configureNext arms nothing.
import { createRoot } from '../../src/renderer.js';
import { View } from 'embedded-react';
import { LayoutAnimation } from 'embedded-react';
import { check, report } from './harness.js';

const ys = {};

function App({ big }) {
  return (
    <View style={{ width: 200, height: 300 }}>
      <View style={{ width: 200, height: big ? 120 : 40 }} onLayout={(e) => (ys.a = e.layout)} />
      <View style={{ width: 200, height: 40 }} onLayout={(e) => (ys.b = e.layout.y)} />
    </View>
  );
}

const root = createRoot({ width: screen.width, height: screen.height });

// Mount: new nodes snap to position — no animation should be pending.
root.render(<App big={false} />);
check(ys.b === 40, `second item starts below the 40px first item (y=${ys.b})`);
check(NativeUI.hasPendingLayoutAnimation() === false, 'no layout animation pending after initial mount');

// Arm a config: it is pending until the next commit consumes it.
LayoutAnimation.configureNext(LayoutAnimation.Presets.easeInEaseOut);
check(NativeUI.hasPendingLayoutAnimation() === true, 'config is armed after configureNext, before commit');

// The change (first item grows 40 -> 120, pushing the second down) commits and consumes the config.
root.render(<App big={true} />);
check(ys.b === 120, `second item's computed target moved to y=120 (got ${ys.b})`);
check(NativeUI.hasPendingLayoutAnimation() === false, 'commit consumed the armed config');

// Control: a layout change WITHOUT configureNext must not arm anything.
root.render(<App big={false} />);
check(ys.b === 40, 'second item snapped back to y=40');
check(NativeUI.hasPendingLayoutAnimation() === false, 'no config armed without configureNext');

// configureNext with an onAnimationDidEnd callback fires it (approximated via a timer).
let ended = false;
LayoutAnimation.configureNext(LayoutAnimation.Presets.linear, () => (ended = true));
root.render(<App big={true} />);
NativeUI.tick(600); // past the 500ms linear preset
check(ended === true, 'onAnimationDidEnd callback fired after the duration');

report('layout-anim');
