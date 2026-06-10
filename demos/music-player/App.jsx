import { useState } from 'react';
import { View, Text, Pressable, StyleSheet } from 'embedded-react';

// Music-player demo. For now it's the minimal counter app that brings up the Flow B AOT compiler
// (useState, static styles, one event handler, interpolated text). As the AOT compiler grows
// (state+events, lists, images, animation) this evolves into a real music-player UI.
export function App() {
  const [count, setCount] = useState(0);

  return (
    <View style={styles.screen}>
      <Text style={styles.title}>Hello, embedded-react</Text>
      <Text style={styles.subtitle}>Flow B — compiled to C, no JS engine</Text>
      <Pressable style={styles.button} onPress={() => setCount((c) => c + 1)}>
        <Text style={styles.buttonText}>Tapped {count} times</Text>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: {
    flex: 1,
    backgroundColor: '#0f172a',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 16,
    padding: 24,
  },
  title: { color: '#f8fafc', fontSize: 24, fontWeight: 'bold' },
  subtitle: { color: '#94a3b8', fontSize: 14 },
  button: { backgroundColor: '#2563eb', paddingHorizontal: 20, paddingVertical: 12, borderRadius: 10 },
  buttonText: { color: '#ffffff', fontSize: 16, fontWeight: 'bold' },
});
