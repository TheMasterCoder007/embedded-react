// Animations: each button starts one shape the C output lowers, and the completions write the status line. A
// timing beside a longer delay waits after it ends; a spring beside one is joined at run time. Stop freezes the
// values it drives, which ends their animations as not finished.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  StyleSheet,
  Animated,
  Easing,
  useAnimatedValue,
} from 'embedded-react';

export function App() {
  const [status, setStatus] = useState('idle');
  const [runs, setRuns] = useState(0);
  const fade = useAnimatedValue(0.2);
  const slide = useAnimatedValue(0);
  const step = useAnimatedValue(0);
  const pulse = useAnimatedValue(0);
  const drift = useAnimatedValue(0);
  const a = useAnimatedValue(1);
  const b = useAnimatedValue(1);
  const c = useAnimatedValue(1);
  const pop = useAnimatedValue(1);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Pressable
          style={styles.button}
          onPress={() => {
            Animated.timing(fade, {
              toValue: 1 - runs * 0.3,
              duration: 400,
              easing: Easing.out(Easing.cubic),
            }).start(({finished}) => {
              setStatus(finished ? 'faded' : 'cut');
              setRuns(runs + 1);
            });
          }}>
          <Text style={styles.label}>fade</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.spring(slide, {
              toValue: 80,
              stiffness: 120,
              damping: 10,
            }).start()
          }>
          <Text style={styles.label}>sprg</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.sequence([
              Animated.timing(step, {toValue: 60, duration: 200}),
              Animated.delay(100),
              Animated.timing(step, {
                toValue: 0,
                duration: 200,
                easing: Easing.bounce,
              }),
            ]).start(() => setStatus('seq done'))
          }>
          <Text style={styles.label}>seq</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.loop(Animated.timing(pulse, {toValue: 1, duration: 150}), {
              iterations: 3,
            }).start(() => setStatus('looped'))
          }>
          <Text style={styles.label}>loop</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.parallel([
              Animated.timing(fade, {toValue: 0}),
              Animated.timing(step, {toValue: 0}),
            ]).stop()
          }>
          <Text style={styles.label}>stop</Text>
        </Pressable>
      </View>
      <View style={styles.row}>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.parallel([
              Animated.spring(pop, {toValue: 1.5}),
              Animated.delay(700),
            ]).start(({finished}) =>
              setStatus(finished ? 'joined' : 'join cut'),
            )
          }>
          <Text style={styles.label}>join</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.parallel([
              Animated.timing(a, {toValue: 0.5, duration: 150}),
              Animated.delay(400),
            ]).start(() => setStatus('waited'))
          }>
          <Text style={styles.label}>wait</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.stagger(80, [
              Animated.timing(a, {toValue: 0.6, duration: 200}),
              Animated.timing(b, {toValue: 0.4, duration: 200}),
              Animated.timing(c, {toValue: 0.8, duration: 200}),
            ]).start()
          }>
          <Text style={styles.label}>stag</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.decay(drift, {velocity: 0.6, deceleration: 0.99}).start()
          }>
          <Text style={styles.label}>dcay</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => Animated.delay(250).start(() => setStatus('slept'))}>
          <Text style={styles.label}>slp</Text>
        </Pressable>
      </View>
      <View style={styles.row}>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.sequence([
              Animated.timing(b, {toValue: 0.3, duration: 100}),
              Animated.delay(300),
            ]).start(({finished}) => setStatus(finished ? 'held' : 'held cut'))
          }>
          <Text style={styles.label}>held</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.loop(
              Animated.sequence([
                Animated.timing(c, {toValue: 0.5, duration: 120}),
                Animated.timing(c, {toValue: 1, duration: 120}),
                Animated.delay(60),
              ]),
            ).start()
          }>
          <Text style={styles.label}>ever</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => Animated.sequence([]).start(() => setStatus('now'))}>
          <Text style={styles.label}>now</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            const shrink = Animated.parallel([
              Animated.spring(pop, {toValue: 1}),
              Animated.delay(600),
            ]);
            shrink.start(({finished}) =>
              setStatus(finished ? 'joined' : 'join cut'),
            );
            shrink.stop();
          }}>
          <Text style={styles.label}>jcut</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Animated.loop(
              Animated.sequence([
                Animated.timing(slide, {toValue: 20, duration: 100}),
                Animated.delay(50),
              ]),
              {iterations: 2},
            ).start(() => setStatus('twice'))
          }>
          <Text style={styles.label}>two</Text>
        </Pressable>
      </View>
      <Text style={styles.status}>{status}</Text>
      <View style={styles.boxes}>
        <View style={[styles.box, {opacity: fade}]} />
        <View style={[styles.box, {transform: [{translateX: slide}]}]} />
        <View style={[styles.box, {transform: [{translateY: step}]}]} />
        <View style={[styles.box, {opacity: pulse}]} />
      </View>
      <View style={styles.boxes}>
        <View style={[styles.box, {transform: [{scale: a}]}]} />
        <View style={[styles.box, {transform: [{scale: b}]}]} />
        <View style={[styles.box, {transform: [{scale: c}]}]} />
        <View style={[styles.box, {transform: [{scale: pop}]}]} />
      </View>
      <View style={[styles.dot, {transform: [{translateX: drift}]}]} />
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
  dot: {width: 16, height: 16, borderRadius: 8, backgroundColor: '#60e080'},
});
