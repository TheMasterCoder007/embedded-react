// <Svg source>: a .svg file with symbols, patterns and an embedded image, kept a vector at both sizes.
import {View, Svg, StyleSheet} from 'embedded-react';
import sampler from './assets/sampler.svg';

export function App() {
  return (
    <View style={styles.root}>
      <Svg source={sampler} width={60} height={60} />
      <Svg source={sampler} style={styles.large} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
  large: {width: 180, height: 180},
});
