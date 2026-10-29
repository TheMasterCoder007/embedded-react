// Drawings: a baked one with a gradient, one sized from the screen, and ones state drives (an arc's sweep, a paint's
// color and width, a gradient used only while a toggle is on, a stop's color, a rounded bar's length), then
// updateVector with a damage hint worked out in ints and one in floats.
import {useState, useRef} from 'react';
import {
  View,
  Text,
  Pressable,
  Svg,
  Circle,
  Rect,
  Path,
  Line,
  Arc,
  Ellipse,
  StyleSheet,
  updateVector,
} from 'embedded-react';

const SW = screen.width;
const HALF = SW / 2;

export function App() {
  const [level, setLevel] = useState(30);
  const [on, setOn] = useState(false);
  const [mode, setMode] = useState('heat');
  const pad = useRef(null);
  const marks = useRef(null);
  const steps = useRef(0);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Svg width={60} height={60}>
          <Circle
            cx={30}
            cy={30}
            r={26}
            fill="#16202f"
            stroke="#f4a261"
            strokeWidth={3}
          />
          <Path
            d="M14 40 L30 14 L46 40 Z"
            fill="#ffffff"
            fillGrad={{
              type: 1,
              ax: 14,
              ay: 14,
              bx: 46,
              by: 40,
              stops: [
                {color: '#39bdf8', offset: 0},
                {color: '#f04741', offset: 1},
              ],
            }}
          />
        </Svg>
        <Svg width={HALF} height={60}>
          <Rect
            x={2}
            y={10}
            width={HALF - 4}
            height={40}
            rx={HALF / 8}
            fill="#203048"
          />
          <Ellipse cx={HALF / 2} cy={30} rx={HALF / 4} ry={12} fill="#60e080" />
        </Svg>
      </View>
      <Pressable style={styles.row} onPress={() => setLevel(level + 25)}>
        <Svg width={100} height={100}>
          <Circle cx={50} cy={50} r={40} fill="#16202f" />
          <Arc
            cx={50}
            cy={50}
            r={40}
            startAngle={-130}
            endAngle={level * 2 - 130}
            stroke={mode === 'cool' ? '#4cc9f0' : '#f4a261'}
            strokeWidth={on ? 12 : 6}
            strokeLinecap="round"
          />
        </Svg>
        <Svg width={120} height={100}>
          <Rect
            x={4}
            y={8}
            width={level}
            height={24}
            rx={on ? 12 : 4}
            fill="#ffffff"
            fillGrad={
              on
                ? {
                    type: 1,
                    ax: 0,
                    ay: 0,
                    bx: level,
                    by: 0,
                    stops: [
                      {color: '#ff6060', offset: 0},
                      {
                        color: mode === 'cool' ? '#4cc9f0' : '#ffc040',
                        offset: 1,
                      },
                    ],
                  }
                : null
            }
          />
          <Line
            x1={4}
            y1={60}
            x2={4 + level}
            y2={60 + level / 10}
            stroke="#b0c0d0"
            strokeWidth={3}
          />
        </Svg>
      </Pressable>
      <View style={styles.row}>
        <Pressable style={styles.button} onPress={() => setOn(!on)}>
          <Text style={styles.label}>{on ? 'on' : 'off'}</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setMode(mode === 'cool' ? 'heat' : 'cool')}>
          <Text style={styles.label}>{mode}</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            steps.current += 1;
            const x = steps.current * 18;
            updateVector(
              pad,
              [
                {rect: [x, 4, 30, 20, 6, 6], fill: '#f4a261'},
                {line: [0, 30, x + 30, 30], stroke: '#e0e0e0', strokeWidth: 2},
              ],
              [x - 18, 0, 48, 32],
            );
          }}>
          <Text style={styles.label}>nudge</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            steps.current += 1;
            const r = steps.current * 2.5;
            updateVector(
              marks,
              [{circle: [20 + r, 16, r / 3], fill: '#80c0ff'}],
              [0, 8, 20 + r * 1.5, 40 / steps.current],
            );
          }}>
          <Text style={styles.label}>dots</Text>
        </Pressable>
      </View>
      <Svg ref={pad} width={200} height={34} />
      <Svg ref={marks} width={200} height={34} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 6},
  row: {flexDirection: 'row', gap: 8},
  button: {
    width: 50,
    height: 30,
    borderRadius: 6,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  label: {fontSize: 12, color: '#ffffff'},
});
