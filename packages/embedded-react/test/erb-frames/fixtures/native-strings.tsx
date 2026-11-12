// Strings and Int64s through the host's probe module, whose interface is in native-probe.ts. greet takes a string
// built in a const and returns one, which another call takes straight back; shift takes and returns an Int64 past an
// Int's range, after an Int; announce has the probe post named on the next frame, with a string and an Int64.
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

export function App() {
  const [count, setCount] = useState(0);
  const [greeting, setGreeting] = useState('none');
  const [size, setSize] = useState(0);
  const [stamp, setStamp] = useState(4000000000);
  const [heard, setHeard] = useState('nobody');
  const [heardAt, setHeardAt] = useState(0);

  useNativeEvent(Probe, 'named', (name, at) => {
    setHeard(name);
    setHeardAt(at);
  });

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            const name = `bob ${count}`;
            const text = Probe.greet(name);
            setGreeting(text);
            setSize(Probe.length(Probe.greet(text)));
            setCount(count + 1);
          }}>
          <Text style={styles.buttonText}>greet</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setStamp(Probe.shift(3, stamp))}>
          <Text style={styles.buttonText}>shift</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => Probe.announce(count)}>
          <Text style={styles.buttonText}>tell</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        {greeting} size {size}
      </Text>
      <Text style={styles.text}>stamp {stamp}</Text>
      <Text style={styles.text}>
        heard {heard} at {heardAt}
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
