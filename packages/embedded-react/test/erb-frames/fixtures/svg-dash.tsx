// Dashed strokes: caps and patterns, closed and curved paths, an offset an animated value drives, and one
// state drives. The button runs the animation and sets the state.
import {useState} from 'react';
import {
  View,
  Text,
  Pressable,
  Svg,
  Circle,
  Rect,
  Line,
  Path,
  G,
  Animated,
  useAnimatedValue,
  StyleSheet,
} from 'embedded-react';

// Round a circle of radius 40.
const AROUND = 251.3;

export function App() {
  const [phase, setPhase] = useState(0);
  const progress = useAnimatedValue(AROUND * 0.75);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Svg width={104} height={104} viewBox="0 0 100 100" stroke="#203040">
          <Line
            x1={8}
            y1={12}
            x2={92}
            y2={12}
            strokeWidth={6}
            strokeDasharray="12 6"
          />
          <Line
            x1={8}
            y1={30}
            x2={92}
            y2={30}
            strokeWidth={6}
            strokeLinecap="round"
            strokeDasharray={[12, 10]}
          />
          <Line
            x1={8}
            y1={48}
            x2={92}
            y2={48}
            strokeWidth={6}
            strokeLinecap="square"
            strokeDasharray="0 12"
          />
          <Line
            x1={8}
            y1={66}
            x2={92}
            y2={66}
            strokeWidth={6}
            strokeLinecap="round"
            strokeDasharray="0 10"
          />
          <Line
            x1={8}
            y1={84}
            x2={92}
            y2={84}
            strokeWidth={4}
            stroke="#e04030"
            strokeDasharray="14 4 2 4"
            strokeDashoffset={5}
          />
        </Svg>
        <Svg width={104} height={104} viewBox="0 0 100 100">
          <G fill="none" strokeWidth={4} strokeDasharray="10 6">
            <Rect
              x={6}
              y={6}
              width={88}
              height={88}
              rx={12}
              stroke="#3060d0"
              strokeDashoffset={-5}
            />
            <Circle
              cx={50}
              cy={50}
              r={28}
              stroke="#30a060"
              strokeLinecap="round"
            />
            <Path
              d="M30 58 Q50 30 70 58"
              stroke="#f0b020"
              strokeDasharray="5 3"
            />
            <Line
              x1={30}
              y1={66}
              x2={70}
              y2={66}
              stroke="#202020"
              strokeDasharray="none"
            />
          </G>
        </Svg>
      </View>
      <View style={styles.row}>
        <Svg
          width={104}
          height={104}
          viewBox="0 0 100 100"
          fill="none"
          strokeWidth={8}>
          <Circle cx={50} cy={50} r={40} stroke="#d0d4d8" />
          <Circle
            cx={50}
            cy={50}
            r={40}
            stroke="#3060d0"
            strokeLinecap="round"
            strokeDasharray={[AROUND, AROUND]}
            strokeDashoffset={progress}
          />
        </Svg>
        <Svg width={104} height={104}>
          <Rect
            x={12}
            y={12}
            width={80}
            height={80}
            fill="none"
            stroke="#e04030"
            strokeWidth={5}
            strokeDasharray="18 10"
            strokeDashoffset={phase}
          />
          <Circle
            cx={52}
            cy={52}
            r={22}
            fill="none"
            stroke="#202020"
            strokeWidth={3}
            strokeDasharray="6 4"
            strokeDashoffset={3}
          />
        </Svg>
      </View>
      <Pressable
        style={styles.button}
        onPress={() => {
          setPhase(14);
          Animated.timing(progress, {
            toValue: AROUND * 0.25,
            duration: 200,
          }).start();
        }}>
        <Text style={styles.label}>go</Text>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, gap: 8, padding: 8, backgroundColor: '#ffffff'},
  row: {flexDirection: 'row', gap: 8},
  button: {
    width: 80,
    height: 36,
    backgroundColor: '#203040',
    alignItems: 'center',
    justifyContent: 'center',
  },
  label: {color: '#ffffff'},
});
