// Switch and TextInput props: track and thumb colors, a default value, a password, a read-only input, and the focus,
// blur and submit events.
import {useState} from 'react';
import {View, Text, TextInput, Switch, StyleSheet} from 'embedded-react';

export function App() {
  const [on, setOn] = useState(true);
  const [last, setLast] = useState('nothing');
  const [submits, setSubmits] = useState(0);

  return (
    <View style={styles.root}>
      <Text style={styles.label}>
        last {last}, submits {submits}
      </Text>
      <View style={styles.row}>
        <Switch
          value={on}
          onValueChange={next => setOn(next)}
          trackColor={{false: '#c04040', true: '#40c040'}}
          thumbColor="#ffff80"
        />
        <Switch style={styles.spaced} value={false} />
      </View>
      <TextInput
        style={styles.input}
        // @ts-expect-error The public types lack defaultValue.
        defaultValue="typed before"
        cursorColor="#ff0000"
        onFocus={() => setLast('focus')}
        onBlur={() => setLast('blur')}
        onSubmitEditing={() => setSubmits(submits + 1)}
      />
      {/* @ts-expect-error The public types lack defaultValue. */}
      <TextInput style={styles.input} defaultValue="secret" secureTextEntry />
      <TextInput style={styles.input} value="read only" editable={false} />
      <TextInput
        style={styles.input}
        placeholder="a hint"
        placeholderTextColor="#40a040"
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#f4f6f8', padding: 8, gap: 6},
  row: {flexDirection: 'row', alignItems: 'center'},
  spaced: {marginLeft: 16},
  input: {
    height: 32,
    borderWidth: 1,
    borderColor: '#9aa8b8',
    paddingLeft: 6,
    color: '#203040',
  },
  label: {fontSize: 16, color: '#203040'},
});
