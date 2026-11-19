// Transforms: entries that are constant, that state drives and that an animated value drives, alone and in one
// array, with a transformOrigin, on a View, a Text and an Svg.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  Svg,
  Rect,
  StyleSheet,
  Animated,
  useAnimatedValue,
} from 'embedded-react';

const TILT = '20deg';

export function App() {
  const [turn, setTurn] = useState(0);
  const [big, setBig] = useState(false);
  const grow = useAnimatedValue(1.25);
  const slide = useAnimatedValue(0);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <View style={[styles.box, styles.turned]} />
        <View
          style={[
            styles.box,
            {
              transform: [{rotate: TILT}, {scaleX: 0.5}],
              transformOrigin: [0, 0],
            },
          ]}
        />
        <View
          style={[
            styles.box,
            {transform: [{rotate: '0.5rad'}, {translateY: 6}]},
          ]}
        />
      </View>
      <View style={styles.row}>
        <View style={[styles.box, {transform: [{rotate: `${turn}deg`}]}]} />
        <View
          style={[
            styles.box,
            {transform: [{scale: big ? 1.3 : 0.8}], transformOrigin: [1, 1]},
          ]}
        />
        <View
          style={[
            styles.box,
            {
              transform: [
                {translateX: turn / 5},
                {rotate: big ? '45deg' : TILT},
              ],
            },
          ]}
        />
      </View>
      <View style={styles.row}>
        <View
          style={[
            styles.box,
            {
              transform: [
                {translateY: 4},
                {rotate: `${turn * 2}deg`},
                {scale: grow},
              ],
              transformOrigin: [0.5, 1],
            },
          ]}
        />
        <Text
          style={[
            styles.label,
            {transform: [{rotate: `${turn}deg`}, {translateX: slide}]},
          ]}>
          turn {turn}
        </Text>
      </View>
      <View style={styles.row}>
        <Svg
          width={48}
          height={48}
          style={{transform: [{rotate: `${turn}deg`}]}}>
          <Rect x={8} y={8} width={32} height={32} fill="#40c080" />
        </Svg>
      </View>
      <View style={styles.row}>
        <Pressable
          style={styles.button}
          onPress={() => {
            setTurn(turn + 30);
            Animated.timing(slide, {toValue: turn + 30, duration: 200}).start();
          }}>
          <Text style={styles.label}>turn</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setBig(!big)}>
          <Text style={styles.label}>size</Text>
        </Pressable>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 12, gap: 16},
  row: {flexDirection: 'row', gap: 28, alignItems: 'center'},
  box: {width: 36, height: 36, backgroundColor: '#e06040'},
  turned: {transform: [{rotate: '45deg'}, {scale: 0.75}]},
  button: {
    width: 80,
    height: 36,
    borderRadius: 6,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  label: {fontSize: 16, color: '#ffffff'},
});
