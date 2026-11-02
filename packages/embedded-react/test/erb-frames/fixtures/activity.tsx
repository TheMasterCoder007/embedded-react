// ActivityIndicator: each size and color, stopped by a prop and by state.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  ActivityIndicator,
  StyleSheet,
} from 'embedded-react';

export function App() {
  const [running, setRunning] = useState(true);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        {/* @ts-expect-error The public types lack size and color. */}
        <ActivityIndicator size="small" color="#ff6040" />
        {/* @ts-expect-error The public types lack size and color. */}
        <ActivityIndicator size="large" color="#40a0ff" />
        {/* @ts-expect-error The public types lack color. */}
        <ActivityIndicator style={styles.spaced} color="#40c080" />
        {/* @ts-expect-error The public types lack color. */}
        <ActivityIndicator animating={false} color="#c080ff" />
        <ActivityIndicator animating={running} />
      </View>
      <Pressable style={styles.button} onPress={() => setRunning(!running)}>
        <Text style={styles.label}>{running ? 'running' : 'stopped'}</Text>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#203040', padding: 12, gap: 12},
  row: {flexDirection: 'row', alignItems: 'center', gap: 8},
  spaced: {marginLeft: 12},
  button: {height: 40, borderRadius: 6, padding: 8, backgroundColor: '#90c0f0'},
  label: {fontSize: 16, color: '#102030'},
});
