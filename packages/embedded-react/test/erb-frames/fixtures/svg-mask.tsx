// <ClipPath> and <Mask>: four bands of colour shown through other shapes, the last under a turned <Svg>.
import {
  View,
  Svg,
  Circle,
  Rect,
  Path,
  G,
  ClipPath,
  Mask,
  StyleSheet,
} from 'embedded-react';

const FADE = {
  type: 1 as const,
  stops: [
    {color: '#ffffff', offset: 0},
    {color: '#000000', offset: 1},
  ],
  ax: 10,
  ay: 0,
  bx: 90,
  by: 0,
};

export function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Svg width={104} height={104} viewBox="0 0 100 100">
          <ClipPath id="disc">
            <Circle cx={50} cy={50} r={44} />
          </ClipPath>
          <ClipPath id="ring" clipRule="evenodd">
            <Path d="M50 2A48 48 0 1 0 50 98A48 48 0 1 0 50 2ZM50 30A20 20 0 1 0 50 70A20 20 0 1 0 50 30Z" />
          </ClipPath>
          <G clipPath="url(#ring)">
            <Rect x={0} y={0} width={100} height={25} fill="#e04030" />
            <Rect x={0} y={25} width={100} height={25} fill="#f0b020" />
            <Rect x={0} y={50} width={100} height={25} fill="#30a060" />
            <Rect x={0} y={75} width={100} height={25} fill="#3060d0" />
          </G>
          <Circle cx={50} cy={50} r={12} fill="#202020" clipPath="url(#disc)" />
        </Svg>
        <Svg width={104} height={104} viewBox="0 0 100 100">
          <Mask id="fade">
            <Rect x={0} y={0} width={100} height={100} fillGradient={FADE} />
          </Mask>
          <G mask="url(#fade)">
            <Rect x={0} y={0} width={100} height={25} fill="#e04030" />
            <Rect x={0} y={25} width={100} height={25} fill="#f0b020" />
            <Rect x={0} y={50} width={100} height={25} fill="#30a060" />
            <Rect x={0} y={75} width={100} height={25} fill="#3060d0" />
          </G>
        </Svg>
      </View>
      <View style={styles.row}>
        <Svg width={104} height={104} viewBox="0 0 100 100">
          <ClipPath id="left">
            <Rect x={0} y={0} width={60} height={100} />
          </ClipPath>
          <Mask id="dots" maskType="alpha">
            <Circle cx={35} cy={50} r={30} fill="#000000" />
            <Circle cx={65} cy={50} r={30} fill="#00000080" />
          </Mask>
          <Mask id="frame">
            <Rect
              x={15}
              y={15}
              width={70}
              height={70}
              fill="#606060"
              stroke="#ffffff"
              strokeWidth={10}
            />
          </Mask>
          <G mask="url(#dots)">
            <Rect x={0} y={0} width={100} height={25} fill="#e04030" />
            <Rect x={0} y={25} width={100} height={25} fill="#f0b020" />
            <Rect x={0} y={50} width={100} height={25} fill="#30a060" />
            <Rect x={0} y={75} width={100} height={25} fill="#3060d0" />
          </G>
          <Rect
            x={0}
            y={0}
            width={100}
            height={100}
            fill="#202020"
            mask="url(#frame)"
            clipPath="url(#left)"
          />
        </Svg>
        <Svg
          width={104}
          height={104}
          viewBox="0 0 100 100"
          style={styles.turned}>
          <ClipPath id="window">
            <Rect x={10} y={30} width={80} height={40} rx={12} />
            <Circle cx={50} cy={50} r={30} />
          </ClipPath>
          <Mask id="shade">
            <Rect x={0} y={0} width={100} height={100} fillGradient={FADE} />
          </Mask>
          <G clipPath="url(#window)" mask="url(#shade)">
            <Rect x={0} y={0} width={100} height={25} fill="#e04030" />
            <Rect x={0} y={25} width={100} height={25} fill="#f0b020" />
            <Rect x={0} y={50} width={100} height={25} fill="#30a060" />
            <Rect x={0} y={75} width={100} height={25} fill="#3060d0" />
          </G>
        </Svg>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, gap: 8, padding: 8, backgroundColor: '#ffffff'},
  row: {flexDirection: 'row', gap: 8},
  turned: {transform: [{rotate: '30deg'}]},
});
