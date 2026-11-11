// Native calls through the host's probe module, whose interface is in native-probe.ts: an Int sum, a number from an
// Int, booleans, a count the module keeps, and a number it stores. Handlers, an effect and a timer call it.
import {useEffect, useState} from 'react';
import {View, Text, Pressable, StyleSheet, nativeModule} from 'embedded-react';
import type {Probe} from './native-probe';

const Probe = nativeModule<Probe>('probe', 1);

export function App() {
  const [total, setTotal] = useState(0);
  const [half, setHalf] = useState(0.0);
  const [even, setEven] = useState(false);
  const [on, setOn] = useState(false);
  const [kept, setKept] = useState(0.0);
  const [ticks, setTicks] = useState(0);

  useEffect(() => {
    setOn(Probe.flip(false));
  }, []);

  useEffect(() => {
    const id = setTimeout(() => setTicks(Probe.add(ticks, 10)), 100);
    return () => clearTimeout(id);
  }, []);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            const step = Probe.next();
            setTotal(value => Probe.add(value, step));
            setHalf(Probe.half(step));
            setEven(Probe.even(step));
            Probe.store(Probe.half(step) * 3);
          }}>
          <Text style={styles.buttonText}>next</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            setOn(Probe.flip(on));
            setKept(Probe.stored());
          }}>
          <Text style={styles.buttonText}>read</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        total {total} half {half} {even ? 'even' : 'odd'}
      </Text>
      <Text style={styles.text}>
        {on ? 'on' : 'off'} kept {kept} ticks {ticks}
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 4},
  bar: {flexDirection: 'row', gap: 4, height: 24},
  button: {
    width: 48,
    height: 24,
    borderRadius: 4,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 10, color: '#ffffff'},
  text: {fontSize: 10, color: '#f0f0f0'},
});
