// Events: press in and out, a long press, disabled presses, raw touches, a layout event, and `visible`.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  TouchableOpacity,
  StyleSheet,
} from 'embedded-react';

export function App() {
  const [pressIn, setPressIn] = useState(0);
  const [pressOut, setPressOut] = useState(0);
  const [long, setLong] = useState(0);
  const [touch, setTouch] = useState('-');
  const [moves, setMoves] = useState(0);
  const [width, setWidth] = useState(0);
  const [hidden, setHidden] = useState(false);
  const [blocked, setBlocked] = useState(0);

  return (
    <View style={styles.root}>
      <Pressable
        style={styles.box}
        delayLongPress={80}
        onPressIn={() => setPressIn(pressIn + 1)}
        onPressOut={() => setPressOut(pressOut + 1)}
        onLongPress={() => setLong(long + 1)}>
        <Text style={styles.label}>
          in {pressIn}, out {pressOut}, long {long}
        </Text>
      </Pressable>
      <View style={styles.row}>
        <Pressable
          style={styles.half}
          disabled
          onPress={() => setBlocked(blocked + 1)}>
          <Text style={styles.label}>off {blocked}</Text>
        </Pressable>
        <TouchableOpacity
          style={styles.half}
          disabled
          onPress={() => setBlocked(blocked + 1)}>
          <Text style={styles.label}>off too</Text>
        </TouchableOpacity>
      </View>
      <View
        style={styles.box}
        onTouchStart={() => setTouch('start')}
        onTouchMove={() => setMoves(moves + 1)}
        onTouchEnd={() => setTouch('end')}>
        <Text style={styles.label}>
          {touch}, moves {moves}
        </Text>
      </View>
      <View
        style={styles.box}
        onLayout={event => setWidth(event.nativeEvent.layout.width)}>
        <Text style={styles.label}>width {width}</Text>
      </View>
      <Pressable style={styles.box} onPress={() => setHidden(!hidden)}>
        <Text style={styles.label}>toggle</Text>
      </Pressable>
      <View style={styles.dot} visible={!hidden} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#f4f6f8', padding: 8, gap: 6},
  row: {flexDirection: 'row', gap: 6},
  box: {height: 40, padding: 8, backgroundColor: '#c0d8f0'},
  half: {flex: 1, height: 40, padding: 8, backgroundColor: '#e0c0c0'},
  dot: {width: 40, height: 40, borderRadius: 20, backgroundColor: '#ff6040'},
  label: {fontSize: 16, color: '#102030'},
});
