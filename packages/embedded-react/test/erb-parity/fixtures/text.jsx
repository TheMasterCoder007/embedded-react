// Text with no state: sizes, weights, line height, spacing, line limits and a TextInput placeholder.
import {View, Text, TextInput, StyleSheet} from 'embedded-react';

export function App() {
  return (
    <View style={styles.root}>
      <Text style={styles.title}>Flow B package</Text>
      <Text style={styles.body}>
        Read in place from flash, checked once at load, and drawn by the same
        engine as the C output.
      </Text>
      <Text style={styles.clipped} numberOfLines={1}>
        This line is far too long to fit on one line of the panel
      </Text>
      <Text style={styles.small}>small and light</Text>
      <Text style={styles.spaced}>SPACED OUT</Text>
      <TextInput
        style={styles.input}
        placeholder="Type here"
        placeholderTextColor="#8090a0"
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#fafafa', padding: 14, gap: 8},
  title: {fontSize: 24, fontWeight: 'bold', color: '#1a1a2e'},
  body: {fontSize: 14, color: '#333344', lineHeight: 20},
  clipped: {fontSize: 16, color: '#aa2244'},
  small: {fontSize: 12, color: '#226644', fontWeight: '300'},
  spaced: {
    fontSize: 18,
    letterSpacing: 4,
    color: '#444444',
    alignSelf: 'center',
  },
  input: {
    height: 36,
    borderWidth: 1,
    borderColor: '#c0c8d0',
    borderRadius: 6,
    paddingHorizontal: 8,
    fontSize: 14,
  },
});
