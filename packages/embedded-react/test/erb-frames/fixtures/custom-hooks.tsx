// Custom hooks: each call is inlined where it is made, with state of its own, so two counters from one hook count
// apart, and each instance of a component that calls a hook gets its own. A hook that calls hooks, one that returns a
// tuple, a result read by key, and an effect in a hook.
import {useCallback, useEffect, useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

function useCounter(start: number, step = 1) {
  const [count, setCount] = useState(start);
  const inc = useCallback(() => setCount(count + step), [count]);
  const reset = useCallback(() => setCount(start), []);
  return {count, inc, reset};
}

function useToggle(initial: boolean): [boolean, () => void] {
  const [on, setOn] = useState(initial);
  const flip = useCallback(() => setOn(!on), [on]);
  return [on, flip];
}

function usePair() {
  const left = useCounter(0);
  const right = useCounter(100, 10);
  return {
    sum: left.count + right.count,
    incLeft: left.inc,
    incRight: right.inc,
  };
}

function useDoubled(value: number) {
  const [doubled, setDoubled] = useState(0);
  useEffect(() => {
    setDoubled(value * 2);
  }, [value]);
  return doubled;
}

function Tally({label}: {label: string}) {
  const {count, inc} = useCounter(0);
  return (
    <Pressable style={styles.button} onPress={inc}>
      <Text style={styles.buttonText}>
        {label} {count}
      </Text>
    </Pressable>
  );
}

export function App() {
  const a = useCounter(1);
  const b = useCounter(5, 5);
  const [on, flip] = useToggle(false);
  const {sum, incLeft, incRight} = usePair();
  const doubled = useDoubled(a.count);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={a.inc}>
          <Text style={styles.buttonText}>a</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={b.inc}>
          <Text style={styles.buttonText}>b</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={a.reset}>
          <Text style={styles.buttonText}>reset</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={flip}>
          <Text style={styles.buttonText}>flip</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={incLeft}>
          <Text style={styles.buttonText}>left</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={incRight}>
          <Text style={styles.buttonText}>right</Text>
        </Pressable>
        <Tally label="x" />
        <Tally label="y" />
      </View>
      <Text style={styles.text}>
        a {a.count} b {b.count} x2 {doubled}
      </Text>
      <Text style={styles.text}>
        {on ? 'on' : 'off'} sum {sum}
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
