// Native modules imported from a package, node_modules/probe-device, as a firmware publishes them: the host's probe,
// whose interface and mock are in the package, and an optional radio that no pass has.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  StyleSheet,
  useNativeEvent,
} from 'embedded-react';
import {Probe, Radio} from 'probe-device';

export function App() {
  const [total, setTotal] = useState(0);
  const [done, setDone] = useState(-1);
  const [half, setHalf] = useState(0.0);
  useNativeEvent(Probe, 'done', (value, halved) => {
    setDone(value);
    setHalf(halved);
  });

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            const step = Probe.next();
            setTotal(value => Probe.add(value, step));
          }}>
          <Text style={styles.buttonText}>next</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            Probe.start(total);
          }}>
          <Text style={styles.buttonText}>start</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>total {total}</Text>
      <Text style={styles.text}>
        done {done} half {half}
      </Text>
      <Text style={styles.text}>{Radio.available ? 'radio' : 'no radio'}</Text>
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
