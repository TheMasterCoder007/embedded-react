// Loops in values: block-bodied useMemos and helpers used as values that loop, in render and in handlers. A memo
// sums, joins with `continue` and `break`, works out a float, reads another memo, or loops over a filtered list; a
// helper returns early from its loop, or counts to a constant; a handler works a looping helper out before the
// statement that reads it. Each instance of a component works its memo out for itself.
import {useMemo, useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

type Item = {id: number; v: number};

function firstOver(items: Item[], limit: number) {
  for (const item of items) {
    if (item.v > limit) return item.id;
  }
  return 0;
}

function biggest(items: Item[]) {
  let best = 0;
  for (const item of items) if (item.v > best) best = item.v;
  return best;
}

function topId(items: Item[]) {
  let top = 0;
  for (const item of items) if (item.id > top) top = item.id;
  return top;
}

function ramp(count: number) {
  let line = '';
  for (let i = 1; i <= count; i++) {
    line += String(i);
    if (i < count) line += '+';
  }
  return line;
}

function Tally({step}: {step: number}) {
  const [n, setN] = useState(0);
  const tally = useMemo(() => {
    let sum = 0;
    for (let i = 0; i < 4; i++) if (i < n) sum += step;
    return sum;
  }, [n, step]);
  return (
    <Pressable style={styles.button} onPress={() => setN(n + 1)}>
      <Text style={styles.buttonText}>t{tally}</Text>
    </Pressable>
  );
}

export function App() {
  const [items, setItems] = useState<Item[]>([
    {id: 1, v: 3},
    {id: 2, v: 4},
    {id: 3, v: 6},
  ]);
  const [limit, setLimit] = useState(4);
  const [last, setLast] = useState(0);

  const total = useMemo(() => {
    let sum = 0;
    for (const item of items) sum += item.v;
    return sum;
  }, [items]);

  const evens = useMemo(() => {
    let line = '';
    let count = 0;
    for (const item of items) {
      if (item.v % 2 === 1) continue;
      if (count === 2) {
        line += '+';
        break;
      }
      line += String(item.id);
      count++;
    }
    return line === '' ? '-' : line;
  }, [items]);

  const mean = useMemo(() => {
    if (items.length === 0) return 0;
    let sum = 0;
    for (const item of items) sum += item.v;
    return sum / items.length;
  }, [items]);

  const above = useMemo(() => {
    let count = 0;
    for (const item of items) if (item.v * items.length > total) count++;
    return count;
  }, [items, total]);

  const big = items.filter(item => item.v > limit);
  const bigSum = useMemo(() => {
    let sum = 0;
    for (const item of big) sum += item.v;
    return sum;
  }, [big]);

  const small = useMemo(() => {
    for (const item of items) if (item.v > 8) return false;
    return true;
  }, [items]);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            const top = topId(items);
            setItems([...items, {id: top + 1, v: (top * 5) % 11}]);
          }}>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setItems(items.slice(0, -1))}>
          <Text style={styles.buttonText}>drop</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setLimit(value => (value + 2) % 10)}>
          <Text style={styles.buttonText}>limit</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setLast(biggest(items) * 10 + firstOver(items, 3))}>
          <Text style={styles.buttonText}>best</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Tally step={2} />
        <Tally step={5} />
      </View>
      <View style={styles.row}>
        {items.map(item => (
          <Text key={item.id} style={styles.text}>
            {item.id}:{item.v}
          </Text>
        ))}
      </View>
      <Text style={styles.text}>
        sum {total} evens {evens} mean {mean.toFixed(2)}
      </Text>
      <Text style={styles.text}>
        above {above} big {bigSum} {small ? 'small' : 'large'}
      </Text>
      <Text style={styles.text}>
        first {firstOver(items, limit)} limit {limit} last {last} {ramp(3)}
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 4},
  bar: {flexDirection: 'row', gap: 4, height: 24},
  row: {flexDirection: 'row', gap: 8, height: 14},
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
