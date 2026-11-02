// Hooks and components: useMemo, useCallback, a component with props and children, the clocks, and cut-off text.
import {useState, useMemo, useCallback} from 'react';
import type {ReactNode} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

const LONG = 'a line of text far too long to fit on one line of this panel';

interface CardProps {
  title: number;
  color: string;
  children: ReactNode;
}

function Card({title, color, children}: CardProps) {
  return (
    <View style={[styles.card, {backgroundColor: color}]}>
      <Text style={styles.label}>count {title}</Text>
      {children}
    </View>
  );
}

export function App() {
  const [count, setCount] = useState(2);
  const doubled = useMemo(() => count * 2, [count]);
  const bump = useCallback(() => setCount(count + 1), [count]);
  const [started, setStarted] = useState(0);
  const [running, setRunning] = useState(0);

  return (
    <View style={styles.root}>
      <Card title={count} color="#c0e0c0">
        <Text style={styles.label}>doubled {doubled}</Text>
      </Card>
      <Pressable style={styles.button} onPress={bump}>
        <Text style={styles.label}>bump</Text>
      </Pressable>
      <Pressable
        style={styles.button}
        onPress={() => {
          setStarted(Date.now() % 100000);
          setRunning(performance.now() % 100000);
        }}>
        <Text style={styles.label}>
          clock {started}, up {running}
        </Text>
      </Pressable>
      <Text style={styles.label} numberOfLines={1}>
        {LONG}
      </Text>
      <Text style={styles.label} numberOfLines={1} ellipsizeMode="clip">
        {LONG}
      </Text>
      <Text style={styles.label} numberOfLines={2}>
        {LONG}
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 6},
  card: {padding: 8, borderRadius: 6, gap: 4},
  button: {height: 34, padding: 6, backgroundColor: '#90c0f0'},
  label: {fontSize: 16, color: '#102030'},
});
