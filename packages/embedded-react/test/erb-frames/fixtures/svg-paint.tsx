// How <Svg> paints: line caps and joins, the miter limit, the fill rule, gradients on a fill and a stroke, paint
// inherited from <Svg> and <G>, a moved <G>, and a rounded <Rect>.
import {
  View,
  Svg,
  Path,
  Circle,
  Ellipse,
  Rect,
  Line,
  Arc,
  G,
  StyleSheet,
} from 'embedded-react';

const STAR = 'M50 5 L79 95 L5 40 L95 40 L21 95 Z';
const LINEAR = {
  type: 1 as const,
  ax: 0,
  ay: 0,
  bx: 100,
  by: 0,
  stops: [
    {offset: 0, color: '#ff0000'},
    {offset: 1, color: '#0000ff'},
  ],
};

export function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Svg width={100} height={70} viewBox="0 0 100 70">
          <Line
            x1={10}
            y1={10}
            x2={90}
            y2={10}
            stroke="#102030"
            strokeWidth={8}
            strokeLinecap="butt"
          />
          <Line
            x1={10}
            y1={30}
            x2={90}
            y2={30}
            stroke="#102030"
            strokeWidth={8}
            strokeLinecap="round"
          />
          <Line
            x1={10}
            y1={50}
            x2={90}
            y2={50}
            stroke="#102030"
            strokeWidth={8}
            strokeLinecap="square"
          />
        </Svg>
        <Svg width={100} height={70} viewBox="0 0 100 70">
          <Path
            d="M5 60 L25 10 L45 60"
            fill="none"
            stroke="#c04020"
            strokeWidth={6}
            strokeLinejoin="miter"
            strokeMiterlimit={2}
          />
          <Path
            d="M35 60 L55 10 L75 60"
            fill="none"
            stroke="#20a040"
            strokeWidth={6}
            strokeLinejoin="round"
          />
          <Path
            d="M60 60 L80 10 L98 60"
            fill="none"
            stroke="#2040c0"
            strokeWidth={6}
            strokeLinejoin="bevel"
          />
        </Svg>
      </View>
      <View style={styles.row}>
        <Svg width={100} height={100} viewBox="0 0 100 100">
          <Path d={STAR} fill="#e0a020" fillRule="evenodd" />
        </Svg>
        <Svg width={100} height={100} viewBox="0 0 100 100">
          <Rect
            x={5}
            y={5}
            width={90}
            height={35}
            rx={10}
            ry={5}
            fillGrad={LINEAR}
          />
          <Ellipse
            cx={50}
            cy={70}
            rx={40}
            ry={20}
            fill="none"
            stroke="#000000"
            strokeGrad={LINEAR}
            strokeWidth={6}
          />
        </Svg>
      </View>
      <View style={styles.row}>
        <Svg
          width={100}
          height={70}
          viewBox="0 0 100 70"
          fill="#6040c0"
          stroke="#000000"
          strokeWidth={2}>
          <Circle cx={20} cy={35} r={15} />
          <G fill="#40c0a0" strokeWidth={4} translateX={40}>
            <Circle cx={20} cy={35} r={15} />
            <G x={0} y={10} stroke="#ff0000">
              <Rect x={40} y={10} width={15} height={15} />
            </G>
          </G>
        </Svg>
        <Svg width={100} height={70} viewBox="0 0 100 70">
          <Arc
            cx={50}
            cy={40}
            r={25}
            startAngle={-90}
            endAngle={90}
            fill="none"
            stroke="#404040"
            strokeWidth={8}
            strokeGrad={LINEAR}
            strokeLinecap="round"
          />
        </Svg>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, gap: 8, padding: 8, backgroundColor: '#ffffff'},
  row: {flexDirection: 'row', gap: 8},
});
