// setKeyboardConfig with only a look of its own, over the engine's built-in layout.
import {useState} from 'react';
import {View, TextInput, StyleSheet, setKeyboardConfig} from 'embedded-react';

setKeyboardConfig({
  rowHeight: 34,
  keyRadius: 8,
  panelColor: '#203048',
  keyColor: '#3060c0',
  labelColor: 'white',
});

export function App() {
  const [text, setText] = useState('');

  return (
    <View style={styles.root}>
      <TextInput
        style={styles.input}
        value={text}
        onChangeText={next => setText(next)}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#f4f6f8', padding: 12},
  input: {
    height: 36,
    borderWidth: 1,
    borderColor: '#9aa8b8',
    borderRadius: 6,
    paddingHorizontal: 8,
    fontSize: 16,
  },
});
