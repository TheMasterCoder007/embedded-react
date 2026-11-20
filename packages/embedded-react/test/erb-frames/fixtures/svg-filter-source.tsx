// <Svg source>: a .svg file with filters. Glows and drop shadows are images under vector shapes; a blur is an image.
import {View, Svg, StyleSheet} from 'embedded-react';
import filters from './assets/filters.svg';

export function App() {
  return (
    <View style={styles.root}>
      <Svg source={filters} width={100} height={100} />
      <Svg source={filters} style={styles.large} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#f4f6f8', padding: 8, gap: 8},
  large: {width: 200, height: 200},
});
