// A fault: the button's handler reads a row the list doesn't have.
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

export function App() {
  const [items] = useState([
    {label: 'one', n: 1},
    {label: 'two', n: 2},
  ]);
  const [picked, setPicked] = useState(0);

  return (
    <View style={styles.root}>
      <Pressable
        style={styles.button}
        onPress={() => {
          const row = items.length + 3;
          setPicked(items[row].n);
        }}>
        <Text style={styles.text}>read</Text>
      </Pressable>
      <Text style={styles.text}>picked {picked}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 4},
  button: {
    width: 48,
    height: 24,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  text: {fontSize: 12, color: '#ffffff'},
});
