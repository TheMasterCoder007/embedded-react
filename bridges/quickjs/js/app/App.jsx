import { useState, useEffect } from 'react';
import { View, Text, Pressable, Animated, Easing } from 'embedded-react';

// Demo: a box whose opacity + translateX are driven by Animated.Values bound to the engine. Tapping
// "Animate" starts timing/spring animations that run entirely in C (native driver) — the only JS
// per interaction is the tap handler; the frames are the engine's.
//
// The "uptime" counter exercises §2 (timers + job queue): a setInterval inside a useEffect ticks
// once a second and the cleanup clears it on unmount — both made possible by the host pump.
export function App() {
  // Lazy-init so the engine value is created once, not on every render.
  const [opacity] = useState(() => new Animated.Value(1));
  const [tx] = useState(() => new Animated.Value(0));
  const [on, setOn] = useState(false);
  const [uptime, setUptime] = useState(0);

  useEffect(() => {
    const id = setInterval(() => setUptime((s) => s + 1), 1000);
    return () => clearInterval(id);
  }, []);

  const toggle = () => {
    const next = !on;
    setOn(next);
    Animated.timing(opacity, { toValue: next ? 0.25 : 1, duration: 400, easing: Easing.easeInOut }).start();
    Animated.spring(tx, { toValue: next ? 130 : 0, stiffness: 130, damping: 13 }).start();
  };

  return (
    <View
      style={{
        flex: 1,
        backgroundColor: '#111927',
        padding: 16,
        gap: 24,
        justifyContent: 'center',
        alignItems: 'center',
      }}
    >
      <Text style={{ color: '#9aa5b1', fontSize: 14 }}>{`uptime: ${uptime}s`}</Text>
      <Animated.View
        style={{
          width: 84,
          height: 84,
          backgroundColor: '#2a9d8f',
          borderRadius: 16,
          opacity,
          transform: [{ translateX: tx }],
        }}
      />
      <Pressable
        onPress={toggle}
        style={{
          backgroundColor: '#264653',
          borderRadius: 10,
          paddingHorizontal: 22,
          height: 44,
          width: 200,
          justifyContent: 'center',
        }}
      >
        <Text style={{ color: 'white', fontSize: 16, fontWeight: 'bold', textAlign: 'center' }}>Animate</Text>
      </Pressable>
    </View>
  );
}
