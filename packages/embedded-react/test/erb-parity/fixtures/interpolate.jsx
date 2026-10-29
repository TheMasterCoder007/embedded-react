// Interpolated binds and press fades: a mount effect spins a value on the engine's own loop, and interpolations map
// it onto a rotation, a clamped opacity, an extended slide and a three-color background. A <TouchableOpacity> dims
// while it is pressed and fades back after, and runs its own handler too.
import {useState, useEffect} from 'react';
import {
  View,
  Text,
  TouchableOpacity,
  StyleSheet,
  Animated,
  Easing,
  useAnimatedValue,
} from 'embedded-react';

export function App() {
  const [presses, setPresses] = useState(0);
  const spin = useAnimatedValue(0);
  const glow = useAnimatedValue(0);

  useEffect(() => {
    Animated.loop(
      Animated.timing(spin, {toValue: 1, duration: 600, easing: Easing.linear}),
    ).start();
  }, []);

  return (
    <View style={styles.root}>
      <View
        style={[
          styles.box,
          {
            transform: [
              {
                rotate: spin.interpolate({
                  inputRange: [0, 1],
                  outputRange: [0, 360],
                }),
              },
            ],
          },
        ]}
      />
      <View
        style={[
          styles.box,
          {
            opacity: spin.interpolate({
              inputRange: [0.2, 0.8],
              outputRange: [1, 0.1],
              extrapolate: 'clamp',
            }),
          },
        ]}
      />
      <View
        style={[
          styles.box,
          {
            transform: [
              {
                translateX: spin.interpolate({
                  inputRange: [0, 0.5],
                  outputRange: [0, 40],
                }),
              },
            ],
          },
        ]}
      />
      <View
        style={[
          styles.wide,
          {
            backgroundColor: spin.interpolate({
              inputRange: [0, 0.5, 1],
              outputRange: ['#ff0000', '#00ff00', '#0000ff80'],
            }),
          },
        ]}
      />
      <TouchableOpacity
        style={styles.button}
        activeOpacity={0.4}
        onPress={() => {
          setPresses(presses + 1);
          Animated.timing(glow, {toValue: presses + 1, duration: 300}).start();
        }}>
        <Text style={styles.label}>pressed {presses}</Text>
      </TouchableOpacity>
      <TouchableOpacity style={styles.button}>
        <Text style={styles.label}>no handler</Text>
      </TouchableOpacity>
      <Text
        style={[
          styles.label,
          {
            color: glow.interpolate({
              inputRange: [0, 2],
              outputRange: ['#ffffff', '#ffd040'],
            }),
          },
        ]}>
        glow
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 10, gap: 8},
  box: {width: 30, height: 30, backgroundColor: '#e06040'},
  wide: {width: 200, height: 24},
  button: {
    width: 140,
    height: 36,
    borderRadius: 6,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  label: {fontSize: 16, color: '#ffffff'},
});
