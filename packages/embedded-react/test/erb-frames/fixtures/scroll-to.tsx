// scrollTo on a ScrollView ref: a scroll whose position is dropped, one that reads where it landed, a read with no
// scroll, and a scroll worked out from a read.
import {useRef, useState} from 'react';
import {
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
  scrollTo,
} from 'embedded-react';
import type {NodeHandle} from 'embedded-react';

const ROWS = [0, 1, 2, 3, 4, 5, 6, 7];

export function App() {
  const list = useRef<NodeHandle>(null);
  const [at, setAt] = useState(0);
  const [most, setMost] = useState(0);
  return (
    <View style={styles.root}>
      <Text style={styles.label}>
        at {at} of {most}
      </Text>
      <View style={styles.buttons}>
        <Pressable
          style={styles.button}
          onPress={() => {
            scrollTo(list.current, {y: 120});
          }}>
          <Text style={styles.label}>down</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            const {y, maxY} = scrollTo(list.current, {y: 9999})!;
            setAt(y);
            setMost(maxY);
          }}>
          <Text style={styles.label}>end</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            const position = scrollTo(list.current)!;
            setAt(position.y);
            setMost(position.maxY);
          }}>
          <Text style={styles.label}>read</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            const {y} = scrollTo(list.current)!;
            if (y > 70) {
              setAt(scrollTo(list.current, {x: 0, y: y - 70})!.y);
            }
          }}>
          <Text style={styles.label}>up</Text>
        </Pressable>
      </View>
      <ScrollView ref={list} style={styles.scroll}>
        {ROWS.map(row => (
          <View
            key={row}
            style={[
              styles.row,
              {backgroundColor: row % 2 ? '#c0d8f0' : '#f0e0c0'},
            ]}>
            <Text style={styles.label}>row {row}</Text>
          </View>
        ))}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
  buttons: {flexDirection: 'row', gap: 6},
  button: {padding: 6, backgroundColor: '#d0e0d0'},
  scroll: {height: 200, padding: 4, borderWidth: 2, borderColor: '#405060'},
  row: {height: 50, padding: 8},
  label: {fontSize: 16, color: '#102030'},
});
