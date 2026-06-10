import { useState } from 'react';
import { View, Text, Pressable, StyleSheet } from 'embedded-react';

// Music-player demo — exercises the Flow B AOT compiler's Phase 3 features: a reusable child component
// (TrackRow), a `.map` over a constant list (unrolled at compile time), and a play/pause toggle
// (useState + dynamic text). Still a static UI; playback/now-playing wiring comes as AOT grows.
const TRACKS = [
  { title: 'Midnight City', artist: 'M83' },
  { title: 'Resonance', artist: 'Home' },
  { title: 'Nightcall', artist: 'Kavinsky' },
];

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

  return (
    <View style={styles.screen}>
      <Text style={styles.kicker}>NOW PLAYING</Text>
      <View style={[styles.art, { opacity: playing ? 1 : 0.45 }]} />
      <Text style={styles.title}>Midnight City</Text>
      <Text style={styles.artist}>M83</Text>
      <Pressable style={[styles.play, { backgroundColor: playing ? '#22c55e' : '#5b6cff' }]} onPress={() => setPlaying((p) => !p)}>
        <Text style={styles.playLabel}>{playing ? 'Pause' : 'Play'}</Text>
      </Pressable>
      {playing && <Text style={styles.badge}>Playing now</Text>}
      <Text style={styles.upNext}>UP NEXT</Text>
      {TRACKS.map((t, i) => (
        <TrackRow key={i} title={t.title} artist={t.artist} />
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: '#0b1020', alignItems: 'center', paddingVertical: 36, paddingHorizontal: 24, gap: 10 },
  kicker: { color: '#7c89a8', fontSize: 12, fontWeight: 'bold', letterSpacing: 2 },
  art: { width: 168, height: 168, borderRadius: 20, backgroundColor: '#5b6cff', marginTop: 6, marginBottom: 8 },
  title: { color: '#f4f7ff', fontSize: 26, fontWeight: 'bold' },
  artist: { color: '#9aa6c4', fontSize: 16 },
  play: { backgroundColor: '#5b6cff', paddingHorizontal: 40, paddingVertical: 12, borderRadius: 24, marginTop: 8, marginBottom: 8 },
  playLabel: { color: '#ffffff', fontSize: 16, fontWeight: 'bold' },
  badge: { color: '#7ee0a0', fontSize: 13, fontWeight: 'bold', letterSpacing: 1 },
  upNext: { color: '#7c89a8', fontSize: 12, fontWeight: 'bold', letterSpacing: 2, marginTop: 8 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 12, width: 280, paddingVertical: 8 },
  rowDot: { width: 10, height: 10, borderRadius: 5, backgroundColor: '#5b6cff' },
  rowText: { flexDirection: 'column' },
  rowTitle: { color: '#e6ecff', fontSize: 16, fontWeight: 'bold' },
  rowArtist: { color: '#8190b0', fontSize: 13 },
});
