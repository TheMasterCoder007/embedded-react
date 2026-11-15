// A greeting to type: "hi" dismisses the keyboard, and the input's onBlur notes it.
import {useState} from 'react';
import {Keyboard, StyleSheet, Text, TextInput, View} from 'embedded-react';

export function App() {
  const [code, setCode] = useState('');
  const [status, setStatus] = useState('idle');

  return (
    <View style={styles.root}>
      <TextInput
        style={styles.input}
        value={code}
        placeholder="say hi"
        onFocus={() => setStatus('typing')}
        onBlur={() => setStatus('done')}
        onChangeText={text => {
          setCode(text);
          if (text === 'hi') {
            Keyboard.dismiss();
          }
        }}
      />
      <Text style={styles.label}>
        {status}: {code}
      </Text>
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
    color: '#203040',
  },
  label: {fontSize: 16, color: '#203040'},
});
