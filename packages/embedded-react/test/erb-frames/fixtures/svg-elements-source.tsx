// <Svg source>: svg-elements' two static drawings, from a .svg file.
import {View, Svg, StyleSheet} from 'embedded-react';
import elements from './assets/elements.svg';

export function App() {
  return (
    <View style={styles.root}>
      <Svg source={elements} width={216} height={104} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8},
});
