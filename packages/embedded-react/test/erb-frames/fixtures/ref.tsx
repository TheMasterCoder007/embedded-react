// No state at all: a text that reads only a ref, which the update sets on its first pass.
import {useRef} from 'react';
import {View, Text, StyleSheet} from 'embedded-react';

export function App() {
  const id = useRef(42);

  return (
    <View style={styles.root}>
      <Text style={styles.body}>id {id.current}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 12},
  body: {fontSize: 16, color: '#202020'},
});
