import { useState } from 'react';
import { View, Text, Pressable, StyleSheet, Animated, useAnimatedValue } from 'embedded-react';

// Music-player demo — exercises the full Flow B AOT subset: child components (TrackRow), a play/pause
// toggle (useState + dynamic text/styles), a state-driven conditional (the "Playing now" badge), a
// RUNTIME-DYNAMIC queue (useState array) you can Add to / Remove from — rows appear and disappear and
// the count updates — and an ANIMATION: the Play button springs down on press and back on release
// (useAnimatedValue + Animated.spring → the engine's native value driver). All compiled to C, no JS engine.
function TrackRow({ title, artist }) {
  return (
    <View style={styles.row}>
      <View style={styles.rowDot} />
      <View style={styles.rowText}>
        <Text style={styles.rowTitle}>{title}</Text>
        <Text style={styles.rowArtist}>{artist}</Text>
      </View>
    </View>
  );
}

export function App() {
  const [playing, setPlaying] = useState(false);
  const [queue, setQueue] = useState([
    { title: 'Midnight City', artist: 'M83' },
    { title: 'Resonance', artist: 'Home' },
  ]);
  const pressScale = useAnimatedValue(1);

  return (
    <View style={styles.screen}>
      <Text style={styles.kicker}>NOW PLAYING</Text>
      <Text style={styles.title}>Midnight City</Text>
      <Text style={styles.artist}>M83</Text>
      <Pressable
        style={[styles.play, { backgroundColor: playing ? '#22c55e' : '#5b6cff', transform: [{ scale: pressScale }] }]}
        onPressIn={() => Animated.spring(pressScale, { toValue: 0.85 }).start()}
        onPressOut={() => Animated.spring(pressScale, { toValue: 1 }).start()}
        onPress={() => setPlaying((p) => !p)}
      >
        <Text style={styles.playLabel}>{playing ? 'Pause' : 'Play'}</Text>
      </Pressable>
      {playing && <Text style={styles.badge}>Playing now</Text>}

      <Text style={styles.upNext}>QUEUE ({queue.length})</Text>
      <View style={styles.controls}>
        <Pressable
          style={styles.ctl}
          onPress={() => setQueue([...queue, { title: queue.length % 2 === 0 ? 'Nightcall' : 'Strobe', artist: queue.length % 2 === 0 ? 'Kavinsky' : 'deadmau5' }])}
        >
          <Text style={styles.ctlLabel}>+ Add</Text>
        </Pressable>
        <Pressable style={styles.ctl} onPress={() => setQueue(queue.slice(0, -1))}>
          <Text style={styles.ctlLabel}>- Remove</Text>
        </Pressable>
      </View>
      {queue.map((t, i) => (
        <TrackRow key={i} title={t.title} artist={t.artist} />
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: '#333333', alignItems: 'center', paddingVertical: 28, paddingHorizontal: 24, gap: 8 },
  kicker: { color: '#7c89a8', fontSize: 12, fontWeight: 'bold', letterSpacing: 2 },
  title: { color: '#f4f7ff', fontSize: 24, fontWeight: 'bold', marginTop: 4 },
  artist: { color: '#9aa6c4', fontSize: 15 },
  play: { paddingHorizontal: 40, paddingVertical: 11, borderRadius: 24, marginTop: 8 },
  playLabel: { color: '#ffffff', fontSize: 16, fontWeight: 'bold' },
  badge: { color: '#7ee0a0', fontSize: 13, fontWeight: 'bold', letterSpacing: 1 },
  upNext: { color: '#7c89a8', fontSize: 12, fontWeight: 'bold', letterSpacing: 2, marginTop: 12 },
  controls: { flexDirection: 'row', gap: 12, marginBottom: 4 },
  ctl: { backgroundColor: '#1c2440', paddingHorizontal: 18, paddingVertical: 8, borderRadius: 16 },
  ctlLabel: { color: '#c7d2fe', fontSize: 14, fontWeight: 'bold' },
  row: { flexDirection: 'row', alignItems: 'center', gap: 12, width: 280, paddingVertical: 7 },
  rowDot: { width: 10, height: 10, borderRadius: 5, backgroundColor: '#5b6cff' },
  rowText: { flexDirection: 'column' },
  rowTitle: { color: '#e6ecff', fontSize: 16, fontWeight: 'bold' },
  rowArtist: { color: '#8190b0', fontSize: 13 },
});
