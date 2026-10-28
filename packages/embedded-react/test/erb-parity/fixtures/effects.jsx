// Effects: one on a number that derives state and restarts a timeout, with a cleanup that clears it; one on
// a string; and mount effects, one with an early return. An effect's write shows at the next update, as in
// the C output.
import {useState, useEffect, useRef} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

export function App() {
  const [count, setCount] = useState(0);
  const [doubled, setDoubled] = useState(0);
  const [echo, setEcho] = useState(-1);
  const [mode, setMode] = useState('idle');
  const [runs, setRuns] = useState(0);
  const [mounted, setMounted] = useState('no');
  const skip = useRef(1);

  useEffect(() => {
    setDoubled(count * 2);
    const id = setTimeout(() => setEcho(count), 60);
    return () => clearTimeout(id);
  }, [count]);

  useEffect(() => {
    setRuns(runs + 1);
  }, [mode]);

  useEffect(() => {
    setMounted('yes');
  }, []);

  useEffect(() => {
    if (skip.current > 0) {
      return;
    }
    setMounted('never');
  }, []);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Pressable style={styles.button} onPress={() => setCount(count + 1)}>
          <Text style={styles.buttonText}>+</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setMode(mode === 'idle' ? 'busy' : 'idle')}>
          <Text style={styles.buttonText}>mode</Text>
        </Pressable>
      </View>
      <Text style={styles.big}>
        {count} doubled {doubled}
      </Text>
      <Text style={styles.label}>echo {echo}</Text>
      <Text style={styles.label}>
        {mode} runs {runs}
      </Text>
      <Text style={styles.label}>mounted {mounted}</Text>
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
});
