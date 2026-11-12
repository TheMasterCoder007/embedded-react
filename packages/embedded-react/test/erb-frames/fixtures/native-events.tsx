// Native events from the host's probe module, whose interface is in native-probe.ts. start posts done on the next
// frame, which the App, a child shown by a condition and each list row subscribe to. A subscriber hears it only while
// it is mounted: the child while shown, a row while the list holds it. The child adds 1 to the number the probe
// stores and each row 10 times its value, which read shows. The first done the child hears also closes a part of it that adds 100,
// which still hears that one, as it was mounted when it came.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  StyleSheet,
  nativeModule,
  useNativeEvent,
} from 'embedded-react';
import type {Probe} from './native-probe';

const Probe = nativeModule<Probe>('probe', 1);

function Inner() {
  useNativeEvent(Probe, 'done', () => Probe.store(Probe.stored() + 100));
  return <Text style={styles.text}>inner</Text>;
}

function Listener() {
  const [open, setOpen] = useState(true);
  useNativeEvent(Probe, 'done', () => {
    Probe.store(Probe.stored() + 1);
    setOpen(false);
  });
  return (
    <View>
      <Text style={styles.text}>listening</Text>
      {open && <Inner />}
    </View>
  );
}

function Row({value}: {value: number}) {
  useNativeEvent(Probe, 'done', () => Probe.store(Probe.stored() + 10 * value));
  return <Text style={styles.text}>row {value}</Text>;
}

export function App() {
  const [last, setLast] = useState(0);
  const [half, setHalf] = useState(0.0);
  const [even, setEven] = useState(false);
  const [refused, setRefused] = useState(0);
  const [shown, setShown] = useState(false);
  const [kept, setKept] = useState(0.0);
  const [rows, setRows] = useState([1]);

  useNativeEvent(Probe, 'done', (value, halfValue, isEven) => {
    setLast(value);
    setHalf(halfValue);
    setEven(isEven);
    setRefused(Probe.refused());
  });

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => Probe.start(last + 1)}>
          <Text style={styles.buttonText}>start</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setShown(on => !on)}>
          <Text style={styles.buttonText}>show</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setRows(list => [...list, list.length + 1])}>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setKept(Probe.stored())}>
          <Text style={styles.buttonText}>read</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        last {last} half {half} {even ? 'even' : 'odd'} refused {refused}
      </Text>
      <Text style={styles.text}>kept {kept}</Text>
      {shown && <Listener />}
      {rows.map(value => (
        <Row key={value} value={value} />
      ))}
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
