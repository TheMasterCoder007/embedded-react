import { useState, useEffect } from 'react';
import { View, Text, Pressable, Animated, Easing, LayoutAnimation, StyleSheet } from 'embedded-react';

// Demo: a box whose opacity + translateX are driven by Animated.Values bound to the engine. Tapping
// "Animate" starts timing/spring animations that run entirely in C (native driver) — the only JS
// per interaction is the tap handler; the frames are the engine's.
//
// Two performance patterns are worth copying:
//   1. The "uptime" counter is its OWN component, so its once-a-second setState re-renders only that
//      leaf — not the whole app. (A top-level counter would re-run every component every second.)
//   2. Static styles live in a module-level StyleSheet, so their object identity is stable across
//      renders, and the reconciler skips re-committing unchanged nodes.

// Isolated leaf: setInterval ticks its own state, so only this <Text> reconciles each second.
function Uptime() {
  const [uptime, setUptime] = useState(0);
  useEffect(() => {
    const id = setInterval(() => setUptime((s) => s + 1), 1000);
    return () => clearInterval(id);
  }, []);
  // Multi-child interpolation + a nested styled span.
  return (
    <Text style={styles.uptime}>
      uptime <Text style={styles.uptimeValue}>{uptime}s</Text>
    </Text>
  );
}

export function App() {
  // Lazy-init so the engine value is created once, not on every render.
  const [opacity] = useState(() => new Animated.Value(1));
  const [tx] = useState(() => new Animated.Value(0));
  const [on, setOn] = useState(false);
  const [expanded, setExpanded] = useState(false);

  const toggle = () => {
    const next = !on;
    setOn(next);
    // Compose the two animations with Animated.parallel and observe completion via .start(cb) —
    // both legs still run in C (native driver); only the start + completion log are JS.
    Animated.parallel([
      Animated.timing(opacity, { toValue: next ? 0.25 : 1, duration: 400, easing: Easing.easeInOut }),
      Animated.spring(tx, { toValue: next ? 130 : 0, stiffness: 130, damping: 13 }),
    ]).start(({ finished }) => console.log('animation', finished ? 'finished' : 'interrupted'));
  };

  const resize = () => {
    // Arm a layout animation, then change a layout prop: the box resizes and the rest of the column
    // reflows smoothly (tweened in C), instead of snapping.
    LayoutAnimation.configureNext(LayoutAnimation.Presets.easeInEaseOut);
    setExpanded((e) => !e);
  };

  return (
    <View style={styles.container}>
      <Uptime />
      <Animated.View
        style={{
          width: expanded ? 160 : 84,
          height: expanded ? 160 : 84,
          backgroundColor: '#2a9d8f',
          borderRadius: 16,
          opacity,
          transform: [{ translateX: tx }],
        }}
      />
      <Pressable onPress={toggle} style={styles.button}>
        <Text style={styles.buttonText}>Animate</Text>
      </Pressable>
      <Pressable onPress={resize} style={styles.button}>
        <Text style={styles.buttonText}>Resize (layout)</Text>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#111927',
    padding: 16,
    gap: 24,
    justifyContent: 'center',
    alignItems: 'center',
  },
  uptime: { color: '#9aa5b1', fontSize: 14 },
  uptimeValue: { color: '#ffd166', fontWeight: 'bold' },
  button: {
    backgroundColor: '#264653',
    borderRadius: 10,
    paddingHorizontal: 22,
    height: 44,
    width: 200,
    justifyContent: 'center',
  },
  buttonText: { color: 'white', fontSize: 16, fontWeight: 'bold', textAlign: 'center' },
});
