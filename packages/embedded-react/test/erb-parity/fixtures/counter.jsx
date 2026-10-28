// State and handlers: a counter, a toggle, a string built from state, a ref, a conditional, and sizes worked
// out from the screen.
import {useState, useRef} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

const SW = screen.width;
const PAD = Math.round(SW / 24);
const BUTTON = (SW - PAD * 4) / 3;

export function App() {
  const [count, setCount] = useState(0);
  const [on, setOn] = useState(false);
  const [label, setLabel] = useState('none yet');
  const presses = useRef(0);

  const bump = step => {
    presses.current += 1;
    setCount(count + step);
    setLabel('last ' + (step > 0 ? 'up' : 'down') + ' at ' + (count + step));
  };

  return (
    <View style={[styles.root, {padding: PAD}]}>
      <Text style={[styles.count, {color: count > 2 ? '#ff6060' : '#f0f0f0'}]}>
        Count: {count}
      </Text>
      <Text style={styles.label}>{label}</Text>
      <Text style={styles.label}>
        presses {presses.current}, half {count / 2}, on {on}
      </Text>
      <View style={styles.row}>
        <Pressable
          style={[styles.button, {width: BUTTON}]}
          onPress={() => bump(-1)}>
          <Text style={styles.buttonText}>-</Text>
        </Pressable>
        <Pressable
          style={[styles.button, {width: BUTTON}]}
          onPress={() => bump(1)}>
          <Text style={styles.buttonText}>+</Text>
        </Pressable>
        <Pressable
          style={[styles.button, {width: BUTTON, opacity: on ? 1 : 0.4}]}
          onPress={() => setOn(!on)}>
          <Text style={styles.buttonText}>{on ? 'on' : 'off'}</Text>
        </Pressable>
      </View>
      {count >= 3 && <View style={styles.badge} />}
      {on ? (
        <Text style={styles.label}>switched on</Text>
      ) : (
        <Text style={styles.label}>switched off</Text>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', gap: 10},
  count: {fontSize: 32, fontWeight: 'bold'},
  label: {fontSize: 16, color: '#b0c0d0'},
  row: {flexDirection: 'row', gap: 12},
  button: {
    height: 48,
    borderRadius: 8,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 24, color: '#ffffff'},
  badge: {width: 40, height: 40, borderRadius: 20, backgroundColor: '#60e080'},
});
