// Animation groups: a completion on a parallel or stagger of several animations runs once they have all ended.
// Cutting one of a parallel's stops the rest, with a completion or without; a stagger's run on.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  StyleSheet,
  Animated,
  useAnimatedValue,
} from 'embedded-react';

export function App() {
  const [status, setStatus] = useState('idle');
  const fade = useAnimatedValue(0.2);
  const slide = useAnimatedValue(0);
  const firstScale = useAnimatedValue(1);
  const secondScale = useAnimatedValue(1);
  const thirdScale = useAnimatedValue(1);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.parallel([
              Animated.timing(slide, {toValue: 80, duration: 400}),
              Animated.timing(fade, {toValue: 1, duration: 200}),
            ]).start(({finished}) => setStatus(finished ? 'all' : 'all cut'))
          }>
          <Text style={styles.label}>all</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.timing(fade, {toValue: 0.5, duration: 50}).start()
          }>
          <Text style={styles.label}>cut</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.stagger(80, [
              Animated.timing(firstScale, {toValue: 0.6, duration: 200}),
              Animated.timing(secondScale, {toValue: 0.4, duration: 200}),
              Animated.timing(thirdScale, {toValue: 0.8, duration: 200}),
            ]).start(({finished}) =>
              setStatus(finished ? 'staggered' : 'stagger cut'),
            )
          }>
          <Text style={styles.label}>stag</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.timing(firstScale, {toValue: 1, duration: 50}).start()
          }>
          <Text style={styles.label}>poke</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.parallel([
              Animated.timing(firstScale, {toValue: 0.5, duration: 100}),
              Animated.timing(secondScale, {toValue: 0.5, duration: 100}),
              Animated.delay(400),
            ]).start(() => setStatus('waited'))
          }>
          <Text style={styles.label}>wait</Text>
        </Pressable>
      </View>
      <View style={styles.row}>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.parallel([
              Animated.timing(slide, {toValue: 80, duration: 400}),
              Animated.timing(fade, {toValue: 1, duration: 200}),
            ]).start()
          }>
          <Text style={styles.label}>bare</Text>
        </Pressable>
      </View>
      <Text style={styles.status}>{status}</Text>
      <View style={styles.boxes}>
        <View style={[styles.box, {opacity: fade}]} />
        <View style={[styles.box, {transform: [{translateX: slide}]}]} />
      </View>
      <View style={styles.boxes}>
        <View style={[styles.box, {transform: [{scale: firstScale}]}]} />
        <View style={[styles.box, {transform: [{scale: secondScale}]}]} />
        <View style={[styles.box, {transform: [{scale: thirdScale}]}]} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 6},
  row: {flexDirection: 'row', gap: 6, height: 30},
  button: {
    width: 40,
    height: 30,
    borderRadius: 4,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  label: {fontSize: 12, color: '#ffffff'},
  status: {fontSize: 16, color: '#f0f0f0'},
  boxes: {flexDirection: 'row', gap: 20, height: 40, paddingLeft: 10},
  box: {width: 30, height: 30, backgroundColor: '#e06040'},
});
