// Lists: `.map` over list state, a pool of rows. Taps add, drop, keep, slice and clear rows, and a row's press
// reads its own item. The first items leave a field out and round a whole number. The dots alternate a child's
// style by index, so their rows don't share one template, and one tap appends two at once.
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

export function App() {
  const [items, setItems] = useState([
    {label: 'one', n: 1, w: 40.5},
    {label: 'two', n: 2.6, w: 60},
    {label: 'three', w: 80.25},
  ]);
  const [dots, setDots] = useState([{size: 6}]);
  const [picked, setPicked] = useState(0);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() =>
            setItems([
              ...items,
              {
                label: 'row ' + items.length,
                n: items.length * 3,
                w: 30 + items.length * 10.25,
              },
            ])
          }>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setItems(items.slice(0, -1))}>
          <Text style={styles.buttonText}>drop</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setItems(items.slice(0, 2))}>
          <Text style={styles.buttonText}>keep</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setItems(items.slice(0, picked - 4))}>
          <Text style={styles.buttonText}>cut</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setItems([])}>
          <Text style={styles.buttonText}>clear</Text>
        </Pressable>
      </View>
      <Text style={styles.label}>
        {items.length} rows, picked {picked}
      </Text>
      {items.map((item, index) => (
        <Pressable
          key={index}
          style={[styles.row, {width: item.w}]}
          onPress={() => setPicked(picked + item.n)}>
          <Text style={styles.rowText}>
            {item.label} {item.n} #{index}
          </Text>
        </Pressable>
      ))}
      <View style={styles.dots}>
        <Pressable
          style={styles.button}
          onPress={() =>
            setDots([...dots, {size: dots.length * 2 + 6}, {size: 4}])
          }>
          <Text style={styles.buttonText}>dots</Text>
        </Pressable>
        {dots.map((dot, index) => (
          <View key={index} style={{width: dot.size, height: dot.size}}>
            <View style={index % 2 ? styles.odd : styles.even} />
          </View>
        ))}
        <Text style={styles.label}>end</Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 4},
  bar: {flexDirection: 'row', gap: 4, height: 24},
  button: {
    width: 40,
    height: 24,
    borderRadius: 4,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 12, color: '#ffffff'},
  label: {fontSize: 12, color: '#b0c0d0'},
  row: {height: 12, backgroundColor: '#406040', justifyContent: 'center'},
  rowText: {fontSize: 10, color: '#f0f0f0'},
  dots: {flexDirection: 'row', gap: 4, alignItems: 'center', height: 24},
  odd: {flex: 1, backgroundColor: '#e06060'},
  even: {flex: 1, backgroundColor: '#60e080'},
});
