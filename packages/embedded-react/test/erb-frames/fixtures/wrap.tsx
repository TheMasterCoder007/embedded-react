// flexWrap: chips that wrap onto more lines, a row a button switches between wrapping and not, and an alert whose
// three buttons are too wide for one line.
import {useState} from 'react';
import {View, Text, Pressable, Alert, StyleSheet} from 'embedded-react';

const CHIPS = ['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'zeta', 'eta'];

export function App() {
  const [wraps, setWraps] = useState(true);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => setWraps(!wraps)}>
          <Text style={styles.label}>{wraps ? 'wrap' : 'nowrap'}</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Alert.alert('Unsaved changes', 'Keep them?', [
              {text: 'Keep editing'},
              {text: 'Discard changes', style: 'destructive'},
              {text: 'Save and close'},
            ])
          }>
          <Text style={styles.label}>ask</Text>
        </Pressable>
      </View>
      <View style={styles.chips}>
        {CHIPS.map(chip => (
          <View key={chip} style={styles.chip}>
            <Text style={styles.label}>{chip}</Text>
          </View>
        ))}
      </View>
      <View style={[styles.chips, {flexWrap: wraps ? 'wrap' : 'nowrap'}]}>
        {CHIPS.map(chip => (
          <View key={chip} style={styles.chip}>
            <Text style={styles.label}>{chip}</Text>
          </View>
        ))}
      </View>
      <View style={styles.reversed}>
        {CHIPS.map(chip => (
          <View key={chip} style={styles.chip}>
            <Text style={styles.label}>{chip}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#e0e8f0', padding: 12, gap: 8},
  bar: {flexDirection: 'row', gap: 8},
  button: {height: 40, borderRadius: 6, padding: 8, backgroundColor: '#90c0f0'},
  chips: {flexDirection: 'row', flexWrap: 'wrap', gap: 4},
  reversed: {flexDirection: 'row', flexWrap: 'wrap-reverse', gap: 4},
  chip: {borderRadius: 4, padding: 4, backgroundColor: '#ffffff'},
  label: {fontSize: 16, color: '#102030'},
});
