/*
 * split-files.tsx in one file, each file's statements where the split app merges them. The two compile to the same
 * program.
 */
import React, {useCallback, useState} from 'react';
import {Pressable, StyleSheet, Text, View} from 'embedded-react';

const colors = {
  background: '#101418',
  card: '#1d2733',
  text: '#ffffff',
  badge: '#e0a030',
};

const LIMIT = 8;

const badgeStyles = StyleSheet.create({
  badge: {marginTop: 8, padding: 4, backgroundColor: colors.badge},
});

function Badge({value}: {value: number}) {
  return (
    <View style={badgeStyles.badge}>
      <Text>{value}</Text>
    </View>
  );
}

interface Props {
  title: string;
  value: number;
  onPress: () => void;
}

const cardStyles = StyleSheet.create({
  card: {padding: 12, borderRadius: 6, backgroundColor: colors.card},
  title: {color: colors.text, fontSize: 14},
});

function Card({title, value, onPress}: Props) {
  return (
    <Pressable style={cardStyles.card} onPress={onPress}>
      <Text style={cardStyles.title}>
        {title}: {value}
      </Text>
    </Pressable>
  );
}

const STEP = 3;

function useCounter(limit: number) {
  const [count, setCount] = useState(0);
  const bump = useCallback(
    () => setCount(previous => (previous + 1) % limit),
    [limit],
  );
  return {count, bump};
}

const styles = StyleSheet.create({
  root: {flex: 1, padding: 8, backgroundColor: colors.background},
  total: {color: colors.text, fontSize: 16},
});

export function App() {
  const {count, bump} = useCounter(LIMIT);
  return (
    <View style={styles.root}>
      <Card title="Count" value={count} onPress={bump} />
      <Badge value={count * STEP} />
      <Text style={styles.total}>{count}</Text>
    </View>
  );
}
