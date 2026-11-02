// <Svg source>: a .svg file baked at build time, at its attribute's size and at its style's.
import {View, Svg, StyleSheet} from 'embedded-react';
import shape from './assets/shape.svg';

export function App() {
  return (
    <View style={styles.root}>
      <Svg source={shape} width={60} height={60} />
      <Svg source={shape} style={styles.large} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
  large: {width: 150, height: 150},
});
