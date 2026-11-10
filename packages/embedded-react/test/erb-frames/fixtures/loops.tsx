// Loops: `for` and `for…of` over bounded data, with `break`, `continue` and `let` locals. A for…of visits a list
// state, a constant array or a split; a for counts to a constant, a list's length or an outer counter; a helper's
// loop can return early, and an effect's loop builds a string.
import {useEffect, useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

const NAMES = ['a', 'b', 'c'];

export function App() {
  const [items, setItems] = useState([
    {id: 1, v: 3},
    {id: 2, v: 4},
    {id: 3, v: 6},
  ]);
  const [total, setTotal] = useState(0);
  const [odd, setOdd] = useState(0);
  const [bars, setBars] = useState('');
  const [log, setLog] = useState('-');
  const [avg, setAvg] = useState('-');
  const [csv] = useState('x,,y,z');
  const [pieces, setPieces] = useState(0);
  const [found, setFound] = useState(-1);

  const mark = (limit: number) => {
    for (const item of items) {
      if (item.v > limit) {
        setFound(item.id);
        return;
      }
    }
    setFound(0);
  };

  useEffect(() => {
    let line = '';
    for (let i = 0; i < 12; i++) {
      if (i >= total) break;
      line += '|';
    }
    setBars(line);
  }, [total]);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            let top = 0;
            for (const item of items) {
              if (item.id > top) top = item.id;
            }
            setItems(previous => [
              ...previous,
              {id: top + 1, v: (top * 7) % 10},
            ]);
          }}>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            let sum = 0;
            for (const {v} of items) sum += v;
            setTotal(sum);
          }}>
          <Text style={styles.buttonText}>sum</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            let count = 0;
            for (let i = 0; i < items.length; i++) {
              if (items[i].v % 2 === 0) continue;
              count++;
              if (count === 2) break;
            }
            setOdd(count);
          }}>
          <Text style={styles.buttonText}>odd</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            let at = -1;
            for (const [index, item] of items.entries()) {
              if (item.v > 5) {
                at = index;
                break;
              }
            }
            if (at >= 0)
              setItems(previous => previous.filter((_, k) => k !== at));
          }}>
          <Text style={styles.buttonText}>drop</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            let word = '';
            for (let i = 0; i < NAMES.length; i++) {
              for (let r = 0; r <= i; r++) word += NAMES[i];
              word += '.';
            }
            setLog(word);
          }}>
          <Text style={styles.buttonText}>word</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            let mean = 0;
            for (const item of items) mean += item.v / items.length;
            setAvg(mean.toFixed(2));
          }}>
          <Text style={styles.buttonText}>avg</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            const parts = csv.split(',');
            let count = 0;
            for (const part of parts) {
              if (part === '') continue;
              count++;
            }
            setPieces(count);
          }}>
          <Text style={styles.buttonText}>split</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            let ids = '';
            for (let i = items.length - 1; i >= 0; i--)
              ids += String(items[i].id);
            setLog(ids);
            mark(4);
          }}>
          <Text style={styles.buttonText}>rev</Text>
        </Pressable>
      </View>
      <View style={styles.row}>
        {items.map(item => (
          <Text key={item.id} style={styles.text}>
            {item.id}:{item.v}
          </Text>
        ))}
      </View>
      <Text style={styles.text}>
        sum {total} odd {odd} {bars}
      </Text>
      <Text style={styles.text}>{log}</Text>
      <Text style={styles.text}>
        avg {avg} pieces {pieces} found {found}
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
