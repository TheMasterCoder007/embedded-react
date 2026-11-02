// Value callbacks: a TextInput's onChangeText and a Switch's onValueChange. Also two values the host feeds,
// which the package lists as inputs.
import {useState} from 'react';
import {
  View,
  Text,
  TextInput,
  Switch,
  StyleSheet,
  useHostValue,
} from 'embedded-react';

export function App() {
  const [name, setName] = useState('');
  const [on, setOn] = useState(false);
  const steps = useHostValue(0);
  const battery = useHostValue(0.5);

  return (
    <View style={styles.root}>
      <TextInput
        style={styles.input}
        value={name}
        placeholder="your name"
        onChangeText={text => setName(text)}
      />
      <Text style={styles.label}>Hello {name}!</Text>
      <View style={styles.row}>
        <Switch value={on} onValueChange={next => setOn(next)} />
        <Text style={styles.label}>{on ? 'enabled' : 'disabled'}</Text>
      </View>
      <Text style={styles.label}>
        steps {steps}, battery {battery}
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
  },
  label: {fontSize: 16, color: '#203040'},
  row: {flexDirection: 'row', alignItems: 'center', gap: 10},
});
