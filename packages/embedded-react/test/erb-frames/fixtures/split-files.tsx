/*
 * An app split across files: a component, a hook, shared constants, a file that passes another on, and a default
 * export. Each file has a `styles` of its own. split-files-one.tsx is the same app in one file.
 */
import React from 'react';
import {StyleSheet, Text, View} from 'embedded-react';
import Badge from './split/Badge';
import {Card, STEP, useCounter} from './split';
import {LIMIT, colors} from './split/theme';

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
