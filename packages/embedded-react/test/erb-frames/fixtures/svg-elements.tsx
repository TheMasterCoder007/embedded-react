// Polygons, polylines, gradients a fill or stroke names, and opacity: two static drawings, which
// svg-elements-source draws from a file, and one state drives.
import {useState} from 'react';
import {
  View,
  Svg,
  Circle,
  Rect,
  G,
  Polygon,
  Polyline,
  Defs,
  LinearGradient,
  RadialGradient,
  Stop,
  StyleSheet,
} from 'embedded-react';

const STAR = '50,6 61,38 95,38 68,58 78,92 50,72 22,92 32,58 5,38 39,38';

export function App() {
  const [warm] = useState(true);
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Svg width={104} height={104} viewBox="0 0 100 100">
          <Defs>
            <LinearGradient id="sky" x1={0} y1={0} x2={0} y2="100%">
              <Stop offset={0} stopColor="#f0b020" />
              <Stop offset="100%" stopColor="#e04030" />
            </LinearGradient>
            <LinearGradient
              id="wire"
              x1={5}
              y1={0}
              x2={95}
              y2={0}
              gradientUnits="userSpaceOnUse">
              <Stop offset={0} stopColor="#3060d0" />
              <Stop offset={1} stopColor="#30a060" stopOpacity={0.4} />
            </LinearGradient>
          </Defs>
          <Polygon
            points={STAR}
            fill="url(#sky)"
            stroke="#202020"
            strokeWidth={2}
            strokeLinejoin="round"
          />
          <Polyline
            points={[5, 96, 30, 80, 50, 96, 70, 80, 95, 96]}
            fill="none"
            stroke="url(#wire)"
            strokeWidth={5}
            strokeLinecap="round"
            strokeLinejoin="bevel"
          />
        </Svg>
        <Svg width={104} height={104} viewBox="0 0 100 100">
          <RadialGradient id="glow" cx="35%" cy="35%" r={0.6}>
            <Stop offset={0} stopColor="#ffffff" />
            <Stop offset={0.5} stopColor="#3060d0" />
            <Stop offset={1} stopColor="#102040" />
          </RadialGradient>
          <Circle cx={30} cy={30} r={26} fill="url(#glow)" />
          <G opacity={0.6}>
            <Rect
              x={40}
              y={40}
              width={50}
              height={50}
              fill="#e04030"
              stroke="#202020"
              strokeWidth={4}
              strokeOpacity={0.5}
            />
            <Rect
              x={20}
              y={60}
              width={40}
              height={30}
              fill="#30a060"
              fillOpacity={0.5}
            />
          </G>
        </Svg>
      </View>
      <View style={styles.row}>
        <Svg width={104} height={104}>
          <LinearGradient
            id="band"
            x1={0}
            y1={0}
            x2={104}
            y2={0}
            gradientUnits="userSpaceOnUse">
            <Stop offset={0} stopColor="#3060d0" />
            <Stop offset={1} stopColor="#f0b020" />
          </LinearGradient>
          <Rect x={4} y={4} width={96} height={40} rx={8} fill="url(#band)" />
          <Polygon
            points="10,96 52,52 94,96"
            fill={warm ? '#e04030' : '#3060d0'}
            opacity={0.5}
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
