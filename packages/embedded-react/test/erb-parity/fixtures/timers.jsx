// Timers: an interval started on mount, timeouts a tap starts (a fractional delay and a 64-bit one), and an
// interval a tap starts and another clears through a ref. The mount's timeouts have delays ToInt32 wraps: a
// negative one and those that wrap past 2^31 fire at once, and one that wraps to 100 fires at 100 ms.
import {useState, useEffect, useRef} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

export function App() {
  const [ticks, setTicks] = useState(0);
  const [fired, setFired] = useState('waiting');
  const [beats, setBeats] = useState(0);
  const [edges, setEdges] = useState(0);
  const beat = useRef(-1);

  useEffect(() => {
    setInterval(() => setTicks(ticks + 1), 100);
    setTimeout(() => setEdges(edges + 1), -5.5);
    setTimeout(() => setEdges(edges + 10), 3e9 + 0.5);
    setTimeout(() => setEdges(edges + 100), 5e9 + 0.5);
    setTimeout(() => setEdges(edges + 1000), Date.now() * 0 - 7);
    setTimeout(() => setEdges(edges + 10000), Date.now() * 0 + 4294967396);
  }, []);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Pressable
          style={styles.button}
          onPress={() => {
            setFired('soon');
            setTimeout(() => setFired('fired'), 150.5);
            setTimeout(() => setFired(fired + ' late'), Date.now() * 0 + 300);
          }}>
          <Text style={styles.buttonText}>later</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            const id = setInterval(() => setBeats(beats + 1), 40);
            beat.current = id;
          }}>
          <Text style={styles.buttonText}>beat</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => clearInterval(beat.current)}>
          <Text style={styles.buttonText}>stop</Text>
        </Pressable>
      </View>
      <Text style={styles.big}>ticks {ticks}</Text>
      <Text style={styles.label}>{fired}</Text>
      <Text style={styles.label}>beats {beats}</Text>
      <Text style={styles.label}>edges {edges}</Text>
      {ticks % 2 === 0 && <View style={styles.dot} />}
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 10, gap: 10},
  row: {flexDirection: 'row', gap: 5, height: 40},
  button: {
    width: 70,
    height: 40,
    borderRadius: 6,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 16, color: '#ffffff'},
  big: {fontSize: 28, color: '#f0f0f0'},
  label: {fontSize: 16, color: '#b0c0d0'},
  dot: {width: 20, height: 20, borderRadius: 10, backgroundColor: '#60e080'},
});
