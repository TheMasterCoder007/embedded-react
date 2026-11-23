// A fault: the second effect sets a state it depends on, so the update never settles. The fault names that line,
// not the effect's last.
import {useState, useEffect} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

export function App() {
  const [count, setCount] = useState(0);
  const [doubled, setDoubled] = useState(0);
  const [spins, setSpins] = useState(0);
  const [checked, setChecked] = useState(0);

  useEffect(() => {
    setDoubled(count * 2);
  }, [count]);

  useEffect(() => {
    if (count > 0) {
      setSpins(spins + 1);
    }
    setChecked(count);
  }, [count, spins]);

  return (
    <View style={styles.root}>
      <Pressable style={styles.button} onPress={() => setCount(count + 1)}>
        <Text style={styles.text}>spin</Text>
      </Pressable>
      <Text style={styles.text}>
        {count} doubled {doubled} spins {spins} checked {checked}
      </Text>
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
