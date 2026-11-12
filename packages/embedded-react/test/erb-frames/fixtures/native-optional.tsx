// Optional native modules: the host's probe, which every pass has, and a radio, which none has (no firmware
// natives, no radio.mock.ts). Render leaves the radio out, a handler checks before it calls, and the radio's event
// never arrives.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  StyleSheet,
  nativeModule,
  useNativeEvent,
} from 'embedded-react';
import type {Int} from 'embedded-react';
import type {Probe} from './native-probe';

interface Radio {
  tune(channel: Int): boolean;
  events: {station(id: Int): void};
}

const Probe = nativeModule.optional<Probe>('probe', 1);
const Radio = nativeModule.optional<Radio>('radio', 1);

function RadioPage() {
  const [tuned, setTuned] = useState(false);
  return (
    <Pressable style={styles.button} onPress={() => setTuned(Radio.tune(7))}>
      <Text style={styles.buttonText}>{tuned ? 'tuned' : 'tune'}</Text>
    </Pressable>
  );
}

export function App() {
  const [total, setTotal] = useState(0);
  const [station, setStation] = useState(-1);
  useNativeEvent(Radio, 'station', id => setStation(id));

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            if (Probe.available) {
              setTotal(value => Probe.add(value, 5));
            }
            if (Radio.available) {
              setTotal(value => value + 1000);
            }
          }}>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        {Radio.available && <RadioPage />}
      </View>
      {Probe.available && <Text style={styles.text}>probe here</Text>}
      <Text style={styles.text}>
        radio {Radio.available ? 'here' : 'missing'} station {station}
      </Text>
      <Text style={styles.text}>total {total}</Text>
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
