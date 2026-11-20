// <Svg source>: fine patterns over large shapes, which fit the tape as one repeated tile.
import {View, Svg, StyleSheet} from 'embedded-react';
import grid from './assets/pattern-grid.svg';

export function App() {
  return (
    <View style={styles.root}>
      <Svg source={grid} width={220} height={220} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8},
});
