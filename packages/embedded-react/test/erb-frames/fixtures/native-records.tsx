// Records through the host's probe module, whose interface is in native-probe.ts. move takes an object state and
// returns a record kept in a const; swap passes an object built in the call, and its setter takes the record; fix
// returns a record with a string, an Int64 after an Int and a record inside, which an object state takes; tell has the
// probe post located on the next frame, with a record the handler destructures and one an object state takes.
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
  const [pos, setPos] = useState({x: 0, y: 0});
  const [fix, setFix] = useState({
    label: 'none',
    id: 0,
    at: 4000000000,
    place: {lat: 0.0, lon: 0.0},
    ok: false,
  });
  const [heard, setHeard] = useState('nobody');
  const [heardAt, setHeardAt] = useState(4000000000);
  const [from, setFrom] = useState({x: 0, y: 0});

  useNativeEvent(Probe, 'located', ({label: name, at: stamp}, point) => {
    setHeard(name);
    setHeardAt(stamp);
    setFrom(point);
  });

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            const next = Probe.move(pos, 3);
            setPos({x: next.x, y: next.y});
          }}>
          <Text style={styles.buttonText}>move</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setPos(Probe.move({x: pos.y, y: pos.x}, 1))}>
          <Text style={styles.buttonText}>swap</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setFix(Probe.fix(pos.x))}>
          <Text style={styles.buttonText}>fix</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => Probe.locate(pos.y)}>
          <Text style={styles.buttonText}>tell</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        pos {pos.x},{pos.y}
      </Text>
      <Text style={styles.text}>
        {fix.label} #{fix.id} at {fix.at} {fix.ok ? 'even' : 'odd'}
      </Text>
      <Text style={styles.text}>
        place {fix.place.lat} {fix.place.lon}
      </Text>
      <Text style={styles.text}>
        heard {heard} at {heardAt} from {from.x},{from.y}
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
