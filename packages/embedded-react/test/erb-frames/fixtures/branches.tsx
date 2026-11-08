// Conditionals that build the branch they pick and destroy the rest: three screens in a ternary chain, a
// component with its own nested conditional, a list built with its screen, an `&&` after the chain, and a
// static node after both. The bars outside the screens drive them, and the home screen has a press of its own.
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

function Home({count, onBump}: {count: number; onBump: () => void}) {
  return (
    <View style={styles.screen}>
      <Text style={styles.big}>count {count}</Text>
      <Pressable style={styles.button} onPress={onBump}>
        <Text style={styles.buttonText}>bump</Text>
      </Pressable>
      {count >= 2 && <View style={styles.badge} />}
      <Text style={styles.label}>home end</Text>
    </View>
  );
}

export function App() {
  const [page, setPage] = useState('home');
  const [count, setCount] = useState(0);
  const [dark, setDark] = useState(false);
  const [items, setItems] = useState([{label: 'item 0'}]);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => setPage('home')}>
          <Text style={styles.buttonText}>home</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setPage('list')}>
          <Text style={styles.buttonText}>list</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setPage('more')}>
          <Text style={styles.buttonText}>more</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => setCount(count + 1)}>
          <Text style={styles.buttonText}>+</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setItems([...items, {label: 'item ' + items.length}])}>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setDark(!dark)}>
          <Text style={styles.buttonText}>dark</Text>
        </Pressable>
      </View>
      {page === 'home' ? (
        <Home count={count} onBump={() => setCount(count + 1)} />
      ) : page === 'list' ? (
        <View style={styles.screen}>
          <Text style={styles.label}>{items.length} items</Text>
          {items.map((item, index) => (
            <View key={index} style={styles.row}>
              <Text style={styles.rowText}>{item.label}</Text>
            </View>
          ))}
          <Text style={styles.label}>list end</Text>
        </View>
      ) : (
        <View
          style={[
            styles.screen,
            {backgroundColor: dark ? '#101010' : '#304060'},
          ]}>
          {dark ? (
            <Text style={styles.big}>dark</Text>
          ) : (
            <Text style={styles.label}>light</Text>
          )}
          <Text style={styles.label}>count {count}</Text>
        </View>
      )}
      {page !== 'home' && <Text style={styles.label}>away from home</Text>}
      <Text style={styles.label}>footer</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 6},
  bar: {flexDirection: 'row', gap: 6, height: 28},
  button: {
    width: 60,
    height: 28,
    borderRadius: 4,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 14, color: '#ffffff'},
  screen: {gap: 4, padding: 6, backgroundColor: '#203048'},
  big: {fontSize: 24, color: '#f0f0f0'},
  label: {fontSize: 14, color: '#b0c0d0'},
  row: {height: 16, backgroundColor: '#406040', justifyContent: 'center'},
  rowText: {fontSize: 12, color: '#f0f0f0'},
  badge: {width: 24, height: 24, borderRadius: 12, backgroundColor: '#60e080'},
});
