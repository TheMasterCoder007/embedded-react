// Block bodies: useMemo with consts, `if`, `switch` and `return`, and updaters that work the new value out in steps,
// for a number, an object, a tagged object and a list. A test on a tag narrows in both. An updater's loop can return
// early, and two updaters in a row each start from what the one before set.
import {useMemo, useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

type Job = {kind: 'idle'} | {kind: 'busy'; pct: number};

export function App() {
  const [n, setN] = useState(0);
  const [p, setP] = useState({x: 0, y: 0});
  const [job, setJob] = useState<Job>({kind: 'idle'});
  const [items, setItems] = useState([1, 2]);

  const grade = useMemo(() => {
    const top = 6;
    if (n > top) return 'high';
    switch (n % 3) {
      case 0:
        return 'zero';
      case 1:
        return 'one';
    }
    return 'two';
  }, [n]);

  const area = useMemo(() => {
    const width = p.x + 1;
    const height = p.y + 1;
    if (width === height) return width * width;
    return width * height;
  }, [p.x, p.y]);

  const status = useMemo(() => {
    if (job.kind === 'idle') return 'idle';
    if (job.pct >= 50) return 'late';
    return 'early';
  }, [job]);

  const size = useMemo(() => {
    const count = items.length;
    if (count === 0) return 'empty';
    if (count < 4) return 'some';
    return 'full';
  }, [items]);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() =>
            setN(value => {
              const next = value + 1;
              if (next > 9) return 0;
              return next;
            })
          }>
          <Text style={styles.buttonText}>step</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setP(previous => {
              if (previous.x >= 2) return {x: 0, y: previous.y + 1};
              return {...previous, x: previous.x + 1};
            })
          }>
          <Text style={styles.buttonText}>walk</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setJob(previous => {
              if (previous.kind === 'idle') return {kind: 'busy', pct: 0};
              if (previous.pct >= 75) return {kind: 'idle'};
              return {kind: 'busy', pct: previous.pct + 25};
            })
          }>
          <Text style={styles.buttonText}>job</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setItems(previous => {
              let sum = 0;
              for (const value of previous) {
                sum += value;
                if (sum > 12) return [];
              }
              return [...previous, sum + 1];
            })
          }>
          <Text style={styles.buttonText}>grow</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            setN(value => {
              if (value >= 4) return 0;
              return value + 1;
            });
            setN(value => {
              const doubled = value * 2;
              return doubled;
            });
          }}>
          <Text style={styles.buttonText}>twice</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setN(value => {
              let total = value;
              for (let step = 1; step <= 3; step++) {
                total += step;
                if (total > 9) return 9;
              }
              return total;
            })
          }>
          <Text style={styles.buttonText}>jump</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        n {n} {grade}
      </Text>
      <Text style={styles.text}>
        p {p.x},{p.y} area {area}
      </Text>
      <Text style={styles.text}>
        job {job.kind === 'busy' ? job.pct : -1} {status}
      </Text>
      <View style={styles.row}>
        {items.map(value => (
          <Text key={value} style={styles.text}>
            {value}
          </Text>
        ))}
      </View>
      <Text style={styles.text}>items {size}</Text>
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
