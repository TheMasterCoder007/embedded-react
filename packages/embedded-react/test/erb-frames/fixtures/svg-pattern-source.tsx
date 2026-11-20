// <Svg source>: patterns whose tiles are cut, turned, skewed, faded, sized by the shape's box or left to overflow.
import {View, Svg, StyleSheet} from 'embedded-react';
import cut from './assets/pattern-cut.svg';
import tiles from './assets/pattern-tiles.svg';

export function App() {
  return (
    <View style={styles.root}>
      <Svg source={cut} width={110} height={110} />
      <Svg source={tiles} width={220} height={220} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
});
