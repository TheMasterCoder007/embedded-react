// Drawings in list rows: each row's bar reads its item and a state outside the list, and rows added after boot
// draw too.
import {useState} from 'react';
import {View, Text, Pressable, Svg, Rect, StyleSheet} from 'embedded-react';

export function App() {
  const [items, setItems] = useState([{w: 30}, {w: 60}]);
  const [on, setOn] = useState(false);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => setItems([...items, {w: 20 + items.length * 25}])}>
          <Text style={styles.label}>add</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setOn(!on)}>
          <Text style={styles.label}>{on ? 'on' : 'off'}</Text>
        </Pressable>
      </View>
      {items.map((item, index) => (
        <View key={index} style={styles.row}>
          <Svg width={200} height={24}>
            <Rect
              x={2}
              y={2}
              width={item.w + (on ? 40 : 0)}
              height={20}
              rx={on ? 10 : 2}
              fill={on ? '#4cc9f0' : '#f4a261'}
            />
          </Svg>
        </View>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 6},
  bar: {flexDirection: 'row', gap: 8},
  row: {height: 24},
  button: {
    width: 50,
    height: 30,
    borderRadius: 6,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  label: {fontSize: 12, color: '#ffffff'},
});
