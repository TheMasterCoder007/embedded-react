import {useState, useEffect} from 'react';
import {
  View,
  Text,
  Pressable,
  Image,
  StyleSheet,
  Animated,
  useAnimatedValue,
} from 'embedded-react';
import logo from './assets/icons/embedded-react.png';

export function App() {
  const [count, setCount] = useState(0);
  const pulse = useAnimatedValue(1);

  // Gently pulse the logo forever — a native-driver animation (runs in the engine, no per-frame JS).
  useEffect(() => {
    const anim = Animated.loop(
      Animated.sequence([
        Animated.timing(pulse, {toValue: 1.12, duration: 800}),
        Animated.timing(pulse, {toValue: 1.0, duration: 800}),
      ]),
    );
    anim.start();
    return () => anim.stop && anim.stop();
  }, []);

  return (
    <View style={styles.root}>
      {/* The card is width:100% but capped by maxWidth, so it fills small panels and centers on big ones. */}
      <View style={styles.card}>
        <Animated.Image
          source={logo}
          style={[styles.logo, {transform: [{scale: pulse}]}]}
        />
        <Text style={styles.title}>embedded-react</Text>
        <Pressable style={styles.button} onPress={() => setCount(c => c + 1)}>
          <Text style={styles.buttonText}>count is {count}</Text>
        </Pressable>
        <View style={styles.hintBox}>
          <Text style={styles.hint}>Edit App.jsx to see your</Text>
          <Text style={styles.hint}>changes hot-reload</Text>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: '#0b0d10',
    alignItems: 'center',
    justifyContent: 'center',
    padding: 14,
  },
  card: {
    width: '100%',
    maxWidth: 360,
    backgroundColor: '#15181d',
    borderRadius: 16,
    borderWidth: 1,
    borderColor: '#2a313c',
    paddingVertical: 22,
    paddingHorizontal: 18,
    alignItems: 'center',
  },
  logo: {width: 60, height: 60, marginBottom: 10},
  title: {color: '#e8edf4', fontSize: 17, marginBottom: 14},
  button: {
    backgroundColor: '#1e2a44',
    borderColor: '#4f86f7',
    borderWidth: 1,
    borderRadius: 10,
    paddingVertical: 9,
    paddingHorizontal: 20,
  },
  buttonText: {color: '#cfe0ff', fontSize: 15},
  hintBox: {alignItems: 'center', marginTop: 16, gap: 2},
  hint: {color: '#5f6b7a', fontSize: 12},
});
