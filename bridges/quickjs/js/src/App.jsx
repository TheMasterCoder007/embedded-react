import { useState } from 'react';
import { View, Text, Pressable } from './components.js';

// Minimal interactive app: a counter that increments on tap. This exercises the full loop —
// initial render, an engine touch event into the JS handler, setState, React re-render, and the
// reconciler committing the changed Text back to the engine.
export function App() {
  const [count, setCount] = useState(0);

  return (
    <View style={{ flex: 1, backgroundColor: '#111927', padding: 16, gap: 12 }}>
      <Text style={{ color: 'white', fontSize: 24 }}>Hello from React</Text>
      <Text style={{ color: '#9aa5b1', fontSize: 16 }}>{'react-reconciler -> NativeUI -> engine'}</Text>
      <Pressable
        style={{
          backgroundColor: '#2a9d8f',
          borderRadius: 10,
          padding: 12,
          height: 52,
          justifyContent: 'center',
        }}
        onPress={() => setCount((c) => c + 1)}
      >
        <Text style={{ color: 'white', fontSize: 18, fontWeight: 'bold' }}>{`Tapped ${count} times`}</Text>
      </Pressable>
    </View>
  );
}
