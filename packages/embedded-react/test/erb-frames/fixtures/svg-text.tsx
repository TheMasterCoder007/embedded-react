// <SvgText>: text baked to outlines at build time, so a dial's letters turn with it.
import {View, Svg, Circle, Line, SvgText, StyleSheet} from 'embedded-react';
import Inter from '../../../../../assets/fonts/Inter-Regular.ttf';
import Bold from '../../../../../assets/fonts/Inter-Bold.ttf';

const CENTER = 50;

export function App() {
  return (
    <View style={styles.root}>
      <Svg width={200} height={200} viewBox="0 0 100 100">
        <Circle
          cx={CENTER}
          cy={CENTER}
          r={46}
          fill="#101820"
          stroke="#607080"
          strokeWidth={2}
        />
        <Line
          x1={CENTER}
          y1={22}
          x2={CENTER}
          y2={CENTER}
          stroke="#ff5040"
          strokeWidth={2}
        />
        <SvgText
          x={CENTER}
          y={18}
          fontFamily={Bold}
          fontSize={12}
          textAnchor="middle"
          fill="#ff5040">
          N
        </SvgText>
        <SvgText
          x={CENTER}
          y={18}
          fontFamily={Inter}
          fontSize={12}
          textAnchor="middle"
          transform="rotate(90 50 50)"
          fill="#ffffff">
          E
        </SvgText>
        <SvgText
          x={CENTER}
          y={18}
          fontFamily={Inter}
          fontSize={12}
          textAnchor="middle"
          transform="rotate(180 50 50)"
          fill="#ffffff">
          S
        </SvgText>
        <SvgText
          x={CENTER}
          y={18}
          fontFamily={Inter}
          fontSize={12}
          textAnchor="middle"
          transform="rotate(270 50 50)"
          fill="#ffffff">
          W
        </SvgText>
      </Svg>
      <Svg width={200} height={30}>
        <SvgText x={0} y={20} fontFamily={Inter} fontSize={20} fill="#102030">
          Lo
        </SvgText>
        <SvgText
          x={200}
          y={20}
          fontFamily={Inter}
          fontSize={20}
          textAnchor="end"
          fill="#102030">
          Hi
        </SvgText>
      </Svg>
      <Svg width={200} height={30}>
        <SvgText
          x={100}
          y={18}
          fontFamily="Inter"
          fontWeight="bold"
          fontSize={14}
          letterSpacing={4}
          textAnchor="middle"
          fill="none"
          stroke="#2060c0">
          MIN
        </SvgText>
      </Svg>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
});
