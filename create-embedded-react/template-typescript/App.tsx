import React, {useState, useEffect} from 'react';
import {
  View,
  Text,
  Pressable,
  StyleSheet,
  Animated,
  useAnimatedValue,
} from 'embedded-react';
import logo from './assets/icons/embedded-react.png';

// The panel size. The host injects `screen` at runtime (Flow A) and the ahead-of-time build bakes it
// in (Flow B). Everything below derives from it with arithmetic and ternaries, so this one file fits
// a 240×240 watch face and an 800×480 panel alike.
const SW = screen.width;
const SH = screen.height;
const SHORT = SW < SH ? SW : SH;
const COMPACT = SHORT < 300; // 240×320, 320×240, 240×280 …
const ROOMY = SHORT >= 460; // 800×480 and up
const TINY = SH < 260; // not enough rows for the tagline

const PAD = COMPACT ? 8 : 20;
const LOGO = COMPACT ? 44 : ROOMY ? 88 : 64;
const TITLE = COMPACT ? 20 : ROOMY ? 32 : 24;
const BODY = COMPACT ? 12 : 16;
const SMALL = COMPACT ? 10 : 12;
const CARD_MAX = ROOMY ? 460 : 380;
const CARD_W = SW - 2 * PAD < CARD_MAX ? SW - 2 * PAD : CARD_MAX;

export function App(): React.JSX.Element {
  const [count, setCount] = useState(0);
  const pulse = useAnimatedValue(1);

  // Pulse the logo forever. The animation runs in the engine, with no per-frame JavaScript.
  useEffect(() => {
    const anim = Animated.loop(
      Animated.sequence([
        Animated.timing(pulse, {toValue: 1.1, duration: 900}),
        Animated.timing(pulse, {toValue: 1.0, duration: 900}),
      ]),
    );
    anim.start();
    return () => anim.stop();
  }, []);

  return (
    <View style={styles.root}>
      <View style={styles.card}>
        <Animated.Image
          source={logo}
          style={[styles.logo, {transform: [{scale: pulse}]}]}
        />
        <View style={styles.title}>
          <Text style={styles.titleLight}>embedded</Text>
          <Text style={styles.titleBold}>React</Text>
        </View>
        {!TINY && (
          <Text style={styles.tagline}>React Native for embedded MCUs</Text>
        )}
        <Pressable
          style={styles.button}
          onPress={() => setCount((c: number) => c + 1)}>
          <Text style={styles.buttonText}>count is {count}</Text>
        </Pressable>
        {!COMPACT && (
          <Text style={styles.hint}>
            Edit App.tsx and save to see it hot-reload.
          </Text>
        )}
        {COMPACT && <Text style={styles.hint}>Edit App.tsx and save</Text>}
        {COMPACT && <Text style={styles.hint}>to see it hot-reload.</Text>}
        <Text style={styles.meta}>
          {SW} × {SH}
        </Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: '#07111f',
    alignItems: 'center',
    justifyContent: 'center',
    padding: PAD,
  },
  card: {
    width: CARD_W,
    alignItems: 'center',
    gap: COMPACT ? 6 : 12,
    padding: COMPACT ? 12 : 24,
    borderRadius: COMPACT ? 12 : 16,
    borderWidth: 1,
    borderColor: '#1f3550',
    backgroundColor: '#0d2035',
  },
  logo: {width: LOGO, height: LOGO},
  title: {flexDirection: 'row', alignItems: 'center'},
  titleLight: {color: '#e6edf5', fontSize: TITLE},
  titleBold: {color: '#38bdf8', fontSize: TITLE, fontWeight: '700'},
  tagline: {color: '#9fc3da', fontSize: BODY},
  button: {
    marginTop: COMPACT ? 2 : 4,
    paddingVertical: COMPACT ? 8 : 10,
    paddingHorizontal: COMPACT ? 18 : 24,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: '#38bdf8',
    backgroundColor: '#0b2a44',
  },
  buttonText: {color: '#bfe6fc', fontSize: BODY, fontWeight: '700'},
  hint: {color: '#5f7a94', fontSize: SMALL},
  meta: {color: '#3d5a75', fontSize: SMALL, letterSpacing: 1},
});
