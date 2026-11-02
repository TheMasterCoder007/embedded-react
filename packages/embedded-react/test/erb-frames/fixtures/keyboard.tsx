// setKeyboardConfig with a layout of its own: two layers, a shift key lit on the second, a space bar, a backspace
// and a done key, in colors and sizes of its own.
import {useState} from 'react';
import {
  View,
  Text,
  TextInput,
  StyleSheet,
  setKeyboardConfig,
} from 'embedded-react';

setKeyboardConfig({
  gridCols: 10,
  rowHeight: 30,
  keyGap: 3,
  keyRadius: 4,
  fontSize: 14,
  panelColor: '#101820',
  keyColor: '#304058',
  keyActiveColor: '#f4a261',
  labelColor: '#f0f0f0',
  layers: [
    [
      [
        {char: 'a', span: 2},
        {char: 'b', span: 2},
        {char: 'c', span: 2},
        {char: 'd', span: 2},
        {char: 'e', span: 2},
      ],
      [
        {label: 'up', layer: 1, span: 2},
        {char: ' ', span: 6},
        {backspace: true, span: 2},
      ],
      [{done: true, label: 'done', span: 10}],
    ],
    [
      [
        {char: 'A', span: 2},
        {char: 'B', span: 2},
        {char: 'C', span: 2},
        {char: 'D', span: 2},
        {char: 'E', span: 2},
      ],
      [
        {label: 'up', layer: 0, span: 2, highlight: true},
        {char: ' ', span: 6},
        {backspace: true, label: 'del', span: 2},
      ],
      [{done: true, span: 10}],
    ],
  ],
});

export function App() {
  const [text, setText] = useState('');

  return (
    <View style={styles.root}>
      <TextInput
        style={styles.input}
        value={text}
        placeholder="type here"
        onChangeText={next => setText(next)}
      />
      <Text style={styles.label}>[{text}]</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#f4f6f8', padding: 12, gap: 10},
  input: {
    height: 36,
    borderWidth: 1,
    borderColor: '#9aa8b8',
    borderRadius: 6,
    paddingHorizontal: 8,
    fontSize: 16,
  },
  label: {fontSize: 16, color: '#203040'},
});
