import { useState } from 'react';
import { View, Text, Pressable } from '../src/components.js';

// Demo: a keyed list that reverses on tap. Reversing reorders the keyed children, which React
// commits as node moves (insertBefore / appendChild) — exercising state, events, and the tree
// mutation path all at once.
const ITEMS = [
  { key: 'a', label: 'Alpha', color: '#2a9d8f' },
  { key: 'b', label: 'Bravo', color: '#e94560' },
  { key: 'c', label: 'Charlie', color: '#f4a261' },
  { key: 'd', label: 'Delta', color: '#9b59b6' },
];

export function App() {
  const [items, setItems] = useState(ITEMS);

  return (
    <View style={{ flex: 1, backgroundColor: '#111927', padding: 16, gap: 10 }}>
      <Text style={{ color: 'white', fontSize: 22 }}>Reorderable list (React keys)</Text>
      <Pressable
        style={{ backgroundColor: '#264653', borderRadius: 8, padding: 10, height: 40, justifyContent: 'center' }}
        onPress={() => setItems((xs) => [...xs].reverse())}
      >
        <Text style={{ color: 'white', fontSize: 16, fontWeight: 'bold' }}>Reverse order</Text>
      </Pressable>
      {items.map((it) => (
        <View
          key={it.key}
          style={{ backgroundColor: it.color, borderRadius: 8, padding: 10, height: 40, justifyContent: 'center' }}
        >
          <Text style={{ color: 'white', fontSize: 16 }}>{it.label}</Text>
        </View>
      ))}
    </View>
  );
}
