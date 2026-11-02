// No state: nested <Text> spans, which the package sets once, and a text that reads only a ref, which the
// update sets on its first pass.
import {useRef} from 'react';
import {View, Text, StyleSheet} from 'embedded-react';

export function App() {
  const id = useRef(42);

  return (
    <View style={styles.root}>
      <Text style={styles.body}>
        Plain <Text style={styles.red}>red </Text>
        <Text style={styles.big}>big</Text>
      </Text>
      <Text style={styles.body}>id {id.current}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 12, gap: 8},
  body: {fontSize: 16, color: '#202020'},
  red: {color: '#d02020', fontWeight: 'bold'},
  big: {fontSize: 24, letterSpacing: 2},
});
