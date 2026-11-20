// <Svg source>: a .svg file with clip-path and mask, kept a vector at both sizes.
import {View, Svg, StyleSheet} from 'embedded-react';
import porthole from './assets/porthole.svg';

export function App() {
  return (
    <View style={styles.root}>
      <Svg source={porthole} width={60} height={60} />
      <Svg source={porthole} style={styles.large} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
  large: {width: 180, height: 180},
});
