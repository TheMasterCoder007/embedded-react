// ScrollView: rows taller than its box, scrolled by a drag, with a press on a row inside it.
import {useState} from 'react';
import {View, Text, Pressable, ScrollView, StyleSheet} from 'embedded-react';

const ROWS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];

export function App() {
  const [picked, setPicked] = useState(-1);

  return (
    <View style={styles.root}>
      <Text style={styles.label}>picked {picked}</Text>
      <ScrollView style={styles.scroll}>
        {ROWS.map(row => (
          <Pressable
            key={row}
            style={[
              styles.row,
              {backgroundColor: row % 2 ? '#c0d8f0' : '#f0e0c0'},
            ]}
            onPress={() => setPicked(row)}>
            <Text style={styles.label}>row {row}</Text>
          </Pressable>
        ))}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
  scroll: {height: 200, padding: 4, borderWidth: 2, borderColor: '#405060'},
  row: {height: 50, padding: 8},
  label: {fontSize: 16, color: '#102030'},
});
